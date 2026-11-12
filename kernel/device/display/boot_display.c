/**-----------------------------------------------------------------------------

 @file    boot_display.c
 @brief   Minimal early eDP mode set for the boot splash
 @details
 @verbatim

  The userspace GPU server cannot run before the scheduler and the initrd are
  ready. This file switches the panel to the native mode during early boot so
  the HNK splash and its progress bar never change resolution later. It owns
  no desktop, compositor or input path.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <arch/x64/cpu.h>
#include <arch/x64/pci.h>
#include <arch/x64/pit.h>
#include <device/display/display_mode.h>
#include <device/display/fb.h>
#include <device/display/gfx.h>
#include <gpu_reg.h>
#include <device/display/skl_display.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/vector.h>
#include <mm/mm.h>

#define PCH_SUNRISE_POINT   0x9D00
#define PCH_LYNX_POINT      0x9C00

static const uint32_t gms_size[] = {
    0, 32, 64, 96, 128, 32, 48, 64, 128, 256,
    96, 160, 224, 352, 448, 480, 512
};

vec_extern(pci_device_t, pci_devices);

static gfx_pci_t gfx_pci;
static gfx_gtt_t gfx_gtt;
static gfx_mem_manager_t gfx_mgr;
static gfx_fb_t gfx_fb;

static int gfx_generation(uint16_t id)
{
    switch (id) {
    case 0x1902: case 0x1906: case 0x190B: case 0x190E:
    case 0x1912: case 0x1916: case 0x191B: case 0x191D: case 0x191E:
    case 0x1921: case 0x1923: case 0x1926: case 0x1927: case 0x192A:
    case 0x192B: case 0x192D: case 0x1932: case 0x193A: case 0x193B:
    case 0x193D:
        return 2;
    case 0x1602: case 0x1606: case 0x160A: case 0x160B: case 0x160D:
    case 0x160E: case 0x1612: case 0x1616: case 0x161A: case 0x161E:
    case 0x1622: case 0x1626: case 0x162A: case 0x162B: case 0x162D:
    case 0x162E:
        return 1;
    case 0x0402: case 0x0406: case 0x040A: case 0x040B: case 0x040E:
    case 0x0412: case 0x0416: case 0x041A: case 0x041E: case 0x0422:
    case 0x0426:
    case 0x0A06: case 0x0A0E: case 0x0A16: case 0x0A1E: case 0x0A26:
    case 0x0D06: case 0x0D16: case 0x0D26:
        return 0;
    default:
        return -1;
    }
}

bool gfx_boot_device(pci_device_t *out)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);

        if (dev.vendor_id == VENDOR_INTEL && gfx_generation(dev.device_id) >= 0) {
            *out = dev;
            return true;
        }
    }
    return false;
}

void gfx_gtt_write_entry(gfx_gtt_t *gtt, uint32_t index, uint64_t phys)
{
    if (gtt->pte64) {
        gtt->entries[index] = (phys & ~(uint64_t)(GTT_PAGE_SIZE - 1))
            | GTT_ENTRY_VALID;
    } else {
        ((volatile uint32_t *)gtt->entries)[index] =
            ((uint32_t)phys & ~(uint32_t)(GTT_PAGE_SIZE - 1))
            | GTT_ENTRY_VALID;
    }
}

bool gfx_alloc(gfx_mem_manager_t *mgr, gfx_gtt_t *gtt, gfx_object_t *obj,
               uint64_t size, uint64_t align)
{
    uint32_t pages = (uint32_t)NUM_PAGES(size);
    uint64_t phys = pmm_get(pages, 0x0, __func__, __LINE__);
    uint64_t gpu_addr = mgr->shared.current;

    if (phys == 0)
        return false;
    if (align > GTT_PAGE_SIZE)
        gpu_addr = (gpu_addr + align - 1) & ~(align - 1);
    if (gpu_addr + PAGE_ALIGN_UP(size) > mgr->shared.top) {
        pmm_free(phys, pages, __func__, __LINE__);
        return false;
    }

    for (uint32_t i = 0; i < pages; i++)
        gfx_gtt_write_entry(gtt, (uint32_t)((gpu_addr >> GTT_PAGE_SHIFT) + i),
                            phys + (uint64_t)i * GTT_PAGE_SIZE);
    (void)gtt->entries[gpu_addr >> GTT_PAGE_SHIFT];
    mgr->shared.current = gpu_addr + PAGE_ALIGN_UP(size);

    memset(obj, 0, sizeof(*obj));
    obj->cpu_addr = (volatile uint8_t *)PHYS_TO_VIRT(phys);
    obj->gfx_addr = gpu_addr;
    obj->phys = phys;
    obj->size = PAGE_ALIGN_UP(size);
    obj->pages = pages;
    return true;
}

static void gfx_attach_fb(const gfx_fb_t *gfb)
{
    fb_info_t *fb = fb_get();
    uint32_t len = gfb->stride * gfb->height;

    fb->addr = (uint8_t *)gfb->obj.cpu_addr;
    fb->width = gfb->width;
    fb->height = gfb->height;
    fb->pitch = gfb->stride;
    fb->backbuffer_len = len;

    uint8_t *bb = kmalloc(len);

    if (bb != NULL) {
        memset(bb, 0, len);
        fb->backbuffer = bb;
    } else {
        fb->backbuffer = fb->addr;
    }
}

static bool gfx_force_wake(gfx_pci_t *pci)
{
    int32_t timeout = 10000;

    while ((gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xffff) != 0) {
        if (--timeout <= 0)
            return false;
        pit_wait(1);
    }
    gfx_outd(pci, FORCE_WAKE_MT, MASKED_ENABLE(1));
    (void)gfx_ind(pci, ECOBUS);

    timeout = 10000;
    while ((gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xffff) == 0) {
        if (--timeout <= 0)
            return false;
        pit_wait(1);
    }
    return true;
}

static void gfx_mem_swizzle(gfx_pci_t *pci)
{
    uint32_t d0 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH0);
    uint32_t d1 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH1);

    if ((d0 & MAD_DIMM_AB_SIZE_MASK) != (d1 & MAD_DIMM_AB_SIZE_MASK))
        return;
    gfx_outd(pci, ARB_CTL,
             gfx_ind(pci, ARB_CTL) | ARB_CTL_TILED_ADDRESS_SWIZZLING);
    gfx_outd(pci, TILE_CTL, gfx_ind(pci, TILE_CTL) | TILE_CTL_SWIZZLE);
    gfx_outd(pci, ARB_MODE, MASKED_ENABLE(ARB_MODE_AS4TS));
}

static bool gfx_mode_from_hw(gfx_pci_t *pci, display_mode_t *mode)
{
    uint32_t ht = gfx_ind(pci, 0x6F000);
    uint32_t hs = gfx_ind(pci, 0x6F008);
    uint32_t vt = gfx_ind(pci, 0x6F00C);
    uint32_t vs = gfx_ind(pci, 0x6F014);
    uint32_t hactive = (ht & 0xffff) + 1;
    uint32_t htotal = ((ht >> 16) & 0xffff) + 1;
    uint32_t vactive = (vt & 0xffff) + 1;
    uint32_t vtotal = ((vt >> 16) & 0xffff) + 1;
    uint32_t hss = (hs & 0xffff) + 1;
    uint32_t hse = ((hs >> 16) & 0xffff) + 1;
    uint32_t vss = (vs & 0xffff) + 1;
    uint32_t vse = ((vs >> 16) & 0xffff) + 1;

    if (hactive < 640 || hactive > 4096 || vactive < 480 || vactive > 4096
        || htotal <= hactive || vtotal <= vactive)
        return false;

    memset(mode, 0, sizeof(*mode));
    mode->valid = true;
    mode->preferred = true;
    mode->hactive = hactive;
    mode->hblank = htotal - hactive;
    mode->hsync_offset = hss > hactive ? hss - hactive : 0;
    mode->hsync_pulse = hse > hss ? hse - hss : 0;
    mode->vactive = vactive;
    mode->vblank = vtotal - vactive;
    mode->vsync_offset = vss > vactive ? vss - vactive : 0;
    mode->vsync_pulse = vse > vss ? vse - vss : 0;
    mode->pitch = hactive * 4;
    return true;
}

bool gfx_boot_is_gen9(uint16_t id)
{
    return gfx_generation(id) == 2;
}

bool gfx_boot_has_64bit_gtt(uint16_t id)
{
    return gfx_generation(id) >= 1;
}

bool gfx_early_active(void)
{
    return gfx_fb.obj.cpu_addr != NULL;
}

bool gfx_init(void)
{
    pci_device_t dev;
    pci_bar_t bar;
    uint32_t id;

    if (!gfx_boot_device(&dev))
        return false;

    id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);
    pci_outw(id, PCI_CONFIG_COMMAND,
             pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

    pci_get_bar(&bar, id, 0);
    if (bar.u.address == NULL || bar.size == 0)
        return false;

    memset(&gfx_pci, 0, sizeof(gfx_pci));
    gfx_pci.id = id;
    gfx_pci.device_id = dev.device_id;
    gfx_pci.gen9 = gfx_generation(dev.device_id) == 2;
    gfx_pci.pte64 = gfx_generation(dev.device_id) >= 1;
    gfx_pci.mmio_bar = (volatile void *)PHYS_TO_VIRT(bar.u.address);
    gfx_pci.gtt_offset = gfx_pci.pte64 ? 8 * 1024 * 1024 : 2 * 1024 * 1024;
    gfx_pci.gtt_size = gfx_pci.gtt_offset;
    gfx_pci.gtt_addr = (volatile uint64_t *)
        ((uint8_t *)gfx_pci.mmio_bar + gfx_pci.gtt_offset);
    vmm_map(NULL, (uint64_t)gfx_pci.mmio_bar, (uint64_t)bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO);

    pci_get_bar(&bar, id, 2);
    if (bar.u.address == NULL || bar.size == 0)
        return false;
    gfx_pci.aperture_bar = (volatile void *)PHYS_TO_VIRT(bar.u.address);
    gfx_pci.aperture_size = bar.size;
    vmm_map(NULL, (uint64_t)gfx_pci.aperture_bar, (uint64_t)bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_DEFAULT);

    uint16_t ggc = pci_inw(id, MGGC0);
    uint32_t bdsm = pci_ind(id, BDSM);
    uint32_t gms = (ggc >> GGC_GMS_SHIFT) & GGC_GMS_MASK;

    memset(&gfx_gtt, 0, sizeof(gfx_gtt));
    gfx_gtt.stolen_mem_size =
        gms < sizeof(gms_size) / sizeof(gms_size[0])
        ? gms_size[gms] * 1024 * 1024 : 0;
    gfx_gtt.stolen_mem_base = bdsm & BDSM_ADDR_MASK;
    gfx_gtt.gtt_mem_size = gfx_pci.gtt_size;
    gfx_gtt.pte64 = gfx_pci.pte64;
    gfx_gtt.num_total_entries = gfx_gtt.gtt_mem_size
        / (gfx_gtt.pte64 ? sizeof(uint64_t) : sizeof(uint32_t));
    gfx_gtt.num_mappable_entries = gfx_pci.aperture_size >> GTT_PAGE_SHIFT;
    gfx_gtt.entries = gfx_pci.gtt_addr;

    memset(&gfx_mgr, 0, sizeof(gfx_mgr));
    gfx_mgr.shared.base = gfx_gtt.stolen_mem_size;
    if (gfx_mgr.shared.base < 64 * 1024 * 1024)
        gfx_mgr.shared.base = 64 * 1024 * 1024;
    gfx_mgr.shared.current = gfx_mgr.shared.base;
    gfx_mgr.shared.top = (uint64_t)gfx_gtt.num_mappable_entries << GTT_PAGE_SHIFT;
    gfx_mgr.priv.base = gfx_mgr.shared.top;
    gfx_mgr.priv.current = gfx_mgr.priv.base;
    gfx_mgr.priv.top = (uint64_t)gfx_gtt.num_total_entries << GTT_PAGE_SHIFT;
    gfx_mgr.gfx_mem_base = gfx_pci.aperture_bar;

    bool fw = gfx_force_wake(&gfx_pci);

    gfx_outd(&gfx_pci, VGA_CONTROL,
             gfx_ind(&gfx_pci, VGA_CONTROL) | VGA_DISABLE);
    if (fw) {
        gfx_mem_swizzle(&gfx_pci);
        gfx_outd(&gfx_pci, FORCE_WAKE_MT, MASKED_DISABLE(1));
        (void)gfx_ind(&gfx_pci, ECOBUS);
    }
    for (uint32_t i = 0; i < FENCE_COUNT; i++)
        gfx_outl(&gfx_pci, FENCE_BASE + i * 8, 0);

    const display_mode_t *boot = display_mode_get_boot();
    display_mode_t hw;
    const display_mode_t *mode = boot;

    if (mode == NULL || !mode->valid || mode->hblank == 0) {
        if (!gfx_mode_from_hw(&gfx_pci, &hw))
            return false;
        mode = &hw;
    }

    if (!skl_edp_set_mode(&gfx_pci, &gfx_mgr, &gfx_gtt, mode, &gfx_fb))
        return false;

    gfx_attach_fb(&gfx_fb);
    klogi("GFX: early mode %ux%u, console framebuffer ready\n",
          gfx_fb.width, gfx_fb.height);
    return true;
}
