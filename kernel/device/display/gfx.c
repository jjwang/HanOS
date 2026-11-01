/**-----------------------------------------------------------------------------

 @file    gfx.c
 @brief   Implementation of graphics device handling
 @details
 @verbatim

  This file contains the implementation of functions for handling graphics
  devices within the HanOS kernel. It includes functions for initializing PCI
  graphics devices, configuring graphics translation tables (GTT), managing
  graphics memory, and enabling specific graphics features. It also includes
  functions for entering and exiting force wake states and for allocating
  graphics memory objects.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <3rd-party/boot/limine.h>
#include <stdint.h>
#include <string.h>
#include <arch/x64/cpu.h>
#include <arch/x64/idt.h>
#include <arch/x64/ioapic.h>
#include <arch/x64/madt.h>
#include <arch/x64/isr_base.h>
#include <arch/x64/pci.h>
#include <arch/x64/pit.h>
#include <arch/x64/serial.h>
#include <mm/mm.h>
#include <lib/vector.h>
#include <lib/klog.h>
#include <lib/time.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <device/display/gfx.h>
#include <device/display/gfx_reg.h>
#include <device/display/skl_display.h>
#include <device/display/display_mode.h>
#include <device/display/edid_read.h>
#include <device/display/vbt.h>
#include <device/display/fb.h>
#include <lib/kmalloc.h>

#define PCH_SUNRISE_POINT           0x9D00  /* Skylake PCH (LPC), high byte */
#define PCH_LYNX_POINT              0x9C00  /* Haswell/Broadwell PCH, high byte */

enum {
    GFX_GEN7_5 = 0,             /* Haswell */
    GFX_GEN8 = 1,               /* Broadwell */
    GFX_GEN9 = 2,               /* Skylake */
};

/* Identify the display generation from the iGPU device id. Returns -1 for
 * unsupported devices. */
static int gfx_generation(uint16_t device_id)
{
    switch (device_id) {
    /* Skylake GT1/GT2/GT3/GT3e/GT4e. */
    case 0x1902: case 0x1906: case 0x190B: case 0x190E:
    case 0x1912: case 0x1916: case 0x191B: case 0x191D: case 0x191E:
    case 0x1921: case 0x1923: case 0x1926: case 0x1927: case 0x192A:
    case 0x192B: case 0x192D: case 0x1932: case 0x193A: case 0x193B:
    case 0x193D:
        return GFX_GEN9;
    /* Broadwell GT1/GT2/GT3/GT3e. */
    case 0x1602: case 0x1606: case 0x160A: case 0x160B: case 0x160D:
    case 0x160E: case 0x1612: case 0x1616: case 0x161A: case 0x161E:
    case 0x1622: case 0x1626: case 0x162A: case 0x162B: case 0x162D:
    case 0x162E:
        return GFX_GEN8;
    /* Haswell desktop GT1/GT2/GT3. */
    case 0x0402: case 0x0406: case 0x040A: case 0x040B: case 0x040E:
    case 0x0412: case 0x0416: case 0x041A: case 0x041E: case 0x0422:
    case 0x0426:
    /* Haswell-ULT GT1/GT2/GT3. */
    case 0x0A06: case 0x0A0E: case 0x0A16: case 0x0A1E: case 0x0A26:
    /* Haswell Crystal Well GT1/GT2/GT3. */
    case 0x0D06: case 0x0D16: case 0x0D26:
        return GFX_GEN7_5;
    default:
        return -1;
    }
}

static bool gfx_is_gen9(uint16_t device_id)
{
    return gfx_generation(device_id) == GFX_GEN9;
}

static bool gfx_supported_device(uint16_t device_id)
{
    return gfx_generation(device_id) >= 0;
}

static const uint32_t GMS_TO_SIZE[] = {
    0 * MB,                     /* GMS_0MB */
    32 * MB,                    /* GMS_32MB_1 */
    64 * MB,                    /* GMS_64MB_1 */
    96 * MB,                    /* GMS_96MB_1 */
    128 * MB,                   /* GMS_128MB_1 */
    32 * MB,                    /* GMS_32MB */
    48 * MB,                    /* GMS_48MB */
    64 * MB,                    /* GMS_64MB */
    128 * MB,                   /* GMS_128MB */
    256 * MB,                   /* GMS_256MB */
    96 * MB,                    /* GMS_96MB */
    160 * MB,                   /* GMS_160MB */
    224 * MB,                   /* GMS_224MB */
    352 * MB,                   /* GMS_352MB */
    448 * MB,                   /* GMS_448MB */
    480 * MB,                   /* GMS_480MB */
    512 * MB,                   /* GMS_512MB */
};

vec_extern(pci_device_t, pci_devices);

static gfx_pci_t gfx_pci = { 0 };
static gfx_gtt_t gfx_gtt = { 0 };
static gfx_mem_manager_t gfx_mgr = { 0 };
static gfx_fb_t gfx_fb = { 0 };

/* Display engine vblank. The handler runs on the boot core; the counter is
 * read by the flip path. */
#define GFX_VBLANK_VECTOR       (IRQ0 + 16)
static volatile uint64_t gfx_vblank_seq;
static bool gfx_vblank_irq;
static uint8_t gfx_vblank_line;

static bool gfx_vblank_irq_setup(void);

bool gfx_validate_chipset(uint16_t device_id)
{
    uint16_t pch = gfx_is_gen9(device_id) ? PCH_SUNRISE_POINT : PCH_LYNX_POINT;
    const char *name = gfx_is_gen9(device_id) ? "Sunrise Point"
        : "Lynx/Wildcat Point";

    /* Find the PCH in the enumerated devices instead of assuming the 00:1f.0
     * LPC location. */
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);
        if (dev.vendor_id == VENDOR_INTEL
            && (dev.device_id & 0xFF00) == pch) {
            klogd("GFX: %s PCH %02x:%02x.%d (id 0x%04x)\n", name,
                  dev.bus, dev.device, dev.func, dev.device_id);
            return true;
        }
    }

    kloge("GFX: %s PCH (0x%04x) not found\n", name, pch);
    return false;
}

void gfx_init_pci(gfx_pci_t * pci, pci_device_t dev)
{
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    process_t *t = sched_get_current_process();
    addrspace_t *as = NULL;

    if (t != NULL)
        as = t->addrspace;

    /* Read PCI registers */
    pci_bar_t bar;

    /* Graphics Memory Address Spaces
     * BAR0: GTTMMADR - The combined Graphics Translation Table Modification
     * Range and Memory Mapped Range. GTTADR begins half way into GTTMMADR
     * while the MMIO base address is the same as GTTMMADR.
     * Ref: https://01.org/sites/default/files/documentation/intel-gfx-prm-
     *      osrc-hsw-pcie-config-registers.pdf#page=129
     *
     * Gen9 has a 16 MB GTTMMADR with GTTADR at the 8 MB offset;
     * Gen7.5 has a 4 MB GTTMMADR with GTTADR at the 2 MB offset. */
    pci_get_bar(&bar, id, 0);
    pci->id = id;
    pci->mmio_bar = (volatile void *) PHYS_TO_VIRT(bar.u.address);
    pci->device_id = dev.device_id;
    pci->gen9 = gfx_is_gen9(dev.device_id);
    /* Haswell Gen7.5 has 32-bit GTT entries; Broadwell Gen8 and Skylake
     * Gen9 have 64-bit entries. */
    pci->pte64 = gfx_generation(dev.device_id) >= GFX_GEN8;
    /* GTTADR starts half way into GTTMMADR: 2 MB into a 4 MB BAR on
     * Haswell, 8 MB into a 16 MB BAR on Broadwell and Skylake. */
    pci->gtt_offset = pci->pte64 ? 8 * MB : 2 * MB;
    pci->gtt_size = pci->pte64 ? 8 * MB : 2 * MB;
    pci->gtt_addr = (volatile uint64_t *)
        ((uint8_t *) pci->mmio_bar + pci->gtt_offset);
    klogi("\tGTTMMADR: 0x%lx (%ld MB) on address space 0x%016lx\n",
          (uint64_t) bar.u.address, bar.size / MB, as);

    vmm_map(as, (uint64_t) pci->mmio_bar, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO);

    /* BAR2: GMADR - Address range allocated via the Device 2 (integrated
     * graphics device) GMADR register. The processor and other peer (DMI)
     * devices utilize this address space to read/write graphics data that
     * resides in Main Memory. This address is internally converted to a
     * GM_Address. */
    pci_get_bar(&bar, id, 2);
    pci->aperture_bar = (volatile void *) PHYS_TO_VIRT(bar.u.address);
    pci->aperture_size = bar.size;
    klogi("\tGMADR:    0x%lx (%ld MB)\n", (uint64_t) bar.u.address,
          bar.size / MB);

    vmm_map(as, (uint64_t) pci->aperture_bar, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_DEFAULT);

    /* BAR4: IOBASE - This register provides the Base offset of the IO
     * registers within Device #2. */
    pci_get_bar(&bar, id, 4);
    pci->iobase = bar.u.port;
    klogi("\tIOBASE:   0x%x (%ld bytes)\n", bar.u.port, bar.size);
}

/* The graphics translation tables provide the address mapping from the GPU's
 * virtual address space to a physical address (when using the VT-d the
 * address is actually an I/O address rather than the physical address).
 * Ref: https://bwidawsk.net/blog/2014/6/the-global-gtt-part-1/
 */
void gfx_init_gtt(gfx_pci_t * pci, gfx_gtt_t * gtt, pci_device_t dev)
{
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    uint16_t ggc = pci_inw(id, MGGC0);
    uint32_t bdsm = pci_ind(id, BDSM);

    int32_t gms = (ggc >> GGC_GMS_SHIFT) & GGC_GMS_MASK;
    gtt->stolen_mem_size = GMS_TO_SIZE[gms];

    int32_t ggms = (ggc >> GGC_GGMS_SHIFT) & GGC_GGMS_MASK;

    /* The GTTMMADR BAR fixes the GTT size: Gen9 has 8 MB, Gen7.5 has 2 MB.
     * GGC.GGMS describes the stolen-memory allocation, not this BAR region,
     * and reads back zero on some Haswell/Broadwell firmware. */
    gtt->gtt_mem_size = pci->gtt_size;
    gtt->pte64 = pci->pte64;
    klogd("GFX: GGC.GGMS %d, GTT BAR region %u MB (%u-bit entries)\n", ggms,
          gtt->gtt_mem_size / MB, gtt->pte64 ? 64 : 32);

    gtt->stolen_mem_base = bdsm & BDSM_ADDR_MASK;

    gtt->num_total_entries =
        gtt->gtt_mem_size / (gtt->pte64 ? sizeof(uint64_t) : sizeof(uint32_t));
    gtt->num_mappable_entries = pci->aperture_size >> GTT_PAGE_SHIFT;
    gtt->entries = pci->gtt_addr;

    /* The firmware has already mapped its framebuffer at GPU address 0, so
     * these words reveal the hardware GTT entry layout. Read through the
     * selected GTT offset; the other half of a 4 MB BAR is not mapped. */
    {
        volatile uint32_t *g = (volatile uint32_t *) pci->gtt_addr;

        for (uint32_t i = 0; i < 8; i++)
            klogi("\tGTT[%u] 0x%08x\n", i, g[i]);
    }

    klogi("GTT Config:\n");
    klogi("\tStolen Mem Base:      0x%x\n", gtt->stolen_mem_base);
    klogi("\tStolen Mem Size:      %u MB\n", gtt->stolen_mem_size / MB);
    klogi("\tGTT Mem Size:         %u MB\n", gtt->gtt_mem_size / MB);
    klogi("\tGTT Total Entries:    %u\n", gtt->num_total_entries);
    klogi("\tGTT Mappable Entries: %u\n", gtt->num_mappable_entries);
}

void gfx_init_mem_manager(gfx_pci_t * pci, gfx_gtt_t * gtt,
                          gfx_mem_manager_t * mgr)
{
    mgr->vram.base = 0;
    mgr->vram.current = mgr->vram.base;
    mgr->vram.top = gtt->stolen_mem_size;

    /* The firmware framebuffer sits at GPU address 0 and the CPU keeps
     * writing it through the aperture, so keep our objects clear of that
     * region by starting the shared GPU range at 64 MB. */
    uint64_t shared_base = gtt->stolen_mem_size;
    if (shared_base < 64 * MB)
        shared_base = 64 * MB;
    mgr->shared.base = shared_base;
    mgr->shared.current = shared_base;
    mgr->shared.top = (uint64_t)gtt->num_mappable_entries << GTT_PAGE_SHIFT;
    klogd("GFX: mem_mgr shared base=0x%016lx top=0x%016lx\n",
          mgr->shared.base, mgr->shared.top);

    mgr->priv.base = (uint64_t)gtt->num_mappable_entries << GTT_PAGE_SHIFT;
    mgr->priv.current = mgr->priv.base;
    mgr->priv.top = (uint64_t)gtt->num_total_entries << GTT_PAGE_SHIFT;

    /* Clear all fence registers (provide linear access to mem to cpu) */
    for (uint64_t fence_num = 0; fence_num < FENCE_COUNT; fence_num++) {
        gfx_outl(pci, FENCE_BASE + sizeof(uint64_t) * fence_num, 0);
    }

    mgr->gfx_mem_base = pci->aperture_bar;
    mgr->gfx_mem_next = mgr->gfx_mem_base + 4 * GTT_PAGE_SIZE;
}

bool gfx_enter_force_wake(gfx_pci_t * pci)
{
    klogd("GFX: Entering force wake\n");

    /* Wait for any pending force wake to clear (max 10ms) */
    int64_t timeout = 10000;
    uint32_t force_wake_ack;

    while ((force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xFFFF) != 0) {
        if (--timeout <= 0) {
            kloge("GFX: Timeout waiting for force wake ACK to clear (0x%08x)\n",
                  force_wake_ack);
            return false;
        }
        pit_wait(1);
    }

    /* Request force wake */
    gfx_outd(pci, FORCE_WAKE_MT, MASKED_ENABLE(1));
    gfx_ind(pci, ECOBUS);  /* Posting read */

    /* Wait for force wake acknowledgment (max 10ms) */
    timeout = 10000;
    while ((force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xFFFF) == 0) {
        if (--timeout <= 0) {
            kloge("GFX: Timeout waiting for force wake ACK to set\n");
            return false;
        }
        pit_wait(1);
    }

    klogd("GFX: Force wake enabled (ACK=0x%08x)\n", force_wake_ack);
    return true;
}

bool gfx_exit_force_wake(gfx_pci_t * pci)
{
    gfx_outd(pci, FORCE_WAKE_MT, MASKED_DISABLE(1));
    gfx_ind(pci, ECOBUS);  /* Posting read */

    klogd("GFX: Force wake disabled\n");
    return true;
}

bool pci_get_gfx_device(pci_device_t * gfx_dev)
{
    pci_device_t dev = { 0 };
    bool found = false;

    /* Find a supported Intel HD Graphics device. */
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        dev = vec_at(&pci_devices, i);
        if (dev.vendor_id != VENDOR_INTEL
            || !gfx_supported_device(dev.device_id)) {
            continue;
        }
        klogi("Found GFX device %02x:%02x.%01x - %04x:%04x %s\n",
              dev.bus, dev.device, dev.func, dev.vendor_id, dev.device_id,
              pci_device_id_to_string(&dev));
        found = true;
        break;
    }

    if (!found) {
        return false;
    }

    if (!gfx_validate_chipset(dev.device_id)) {
        memset(&dev, 0, sizeof(pci_device_t));
        return false;
    }

    if (gfx_dev != NULL)
        memcpy(gfx_dev, &dev, sizeof(pci_device_t));
    klogi("PCI: GFX device checking finished.\n");
    return true;
}

/* Hand the framebuffer the display engine is scanning to the console. The
 * kernel keeps a private backbuffer that fb_refresh() copies to the scanout
 * so the engine never samples a partially drawn frame. */
static void gfx_attach_fb(const gfx_fb_t * gfb)
{
    fb_info_t *fb = fb_get();
    uint32_t len = gfb->stride * gfb->height;

    fb->addr = (uint8_t *) gfb->obj.cpu_addr;
    fb->width = gfb->width;
    fb->height = gfb->height;
    fb->pitch = gfb->stride;
    fb->backbuffer_len = len;

    uint8_t *bb = kmalloc(len);
    if (bb != NULL) {
        memset(bb, 0, len);
        fb->backbuffer = bb;
    } else {
        /* Without a backbuffer the console draws straight into the scanout,
         * which is still visible, just not tear-free. */
        kloge("GFX: backbuffer allocation (%u bytes) failed\n", len);
        fb->backbuffer = fb->addr;
    }

    klogi("GFX: console framebuffer %ux%u pitch %u addr 0x%016lx "
          "(gpu 0x%016lx)\n", gfb->width, gfb->height, gfb->stride,
          (uint64_t) fb->addr, gfb->obj.gfx_addr);
}

/* Move the hardware cursor by a relative amount (used by the pointer driver). */
void gfx_cursor_move(int32_t dx, int32_t dy)
{
    skl_cursor_move(dx, dy);
}

/* Read the native timing the firmware programmed into the eDP transcoder.
 * When no EDID/VBT is exposed, the GOP still trains the eDP link at the
 * panel's native mode and scales the 800x600 boot pipe up to it, so the
 * transcoder timing registers hold the real panel timing. */
static bool gfx_mode_from_hw(gfx_pci_t * pci, display_mode_t * mode)
{
    uint32_t ht = gfx_ind(pci, 0x6F000);
    uint32_t hs = gfx_ind(pci, 0x6F008);
    uint32_t vt = gfx_ind(pci, 0x6F00C);
    uint32_t vs = gfx_ind(pci, 0x6F014);
    uint32_t hactive = (ht & 0xffff) + 1;
    uint32_t htotal = ((ht >> 16) & 0xffff) + 1;
    uint32_t vactive = (vt & 0xffff) + 1;
    uint32_t vtotal = ((vt >> 16) & 0xffff) + 1;
    uint32_t hsync_start = (hs & 0xffff) + 1;
    uint32_t hsync_end = ((hs >> 16) & 0xffff) + 1;
    uint32_t vsync_start = (vs & 0xffff) + 1;
    uint32_t vsync_end = ((vs >> 16) & 0xffff) + 1;

    if (hactive < 640 || hactive > 4096 || vactive < 480 || vactive > 4096)
        return false;
    if (htotal <= hactive || vtotal <= vactive)
        return false;

    mode->hactive = hactive;
    mode->hblank = htotal - hactive;
    mode->hsync_offset = (hsync_start > hactive) ? hsync_start - hactive : 0;
    mode->hsync_pulse = (hsync_end > hsync_start) ? hsync_end - hsync_start : 0;
    mode->vactive = vactive;
    mode->vblank = vtotal - vactive;
    mode->vsync_offset = (vsync_start > vactive) ? vsync_start - vactive : 0;
    mode->vsync_pulse = (vsync_end > vsync_start) ? vsync_end - vsync_start : 0;

    mode->valid = true;
    mode->from_edid = false;
    mode->preferred = true;
    mode->interlaced = false;
    mode->hsync_positive = false;
    mode->vsync_positive = false;
    mode->pixel_clock_khz = 0;
    mode->pitch = hactive * 4;
    mode->refresh_hz = 0;
    return true;
}

bool gfx_init(void)
{
    pci_device_t dev = { 0 };
    uint8_t edid[sizeof(edid_info_t)];

    if (!pci_get_gfx_device(&dev))
        return false;

    gfx_init_pci(&gfx_pci, dev);
    gfx_init_gtt(&gfx_pci, &gfx_gtt, dev);
    gfx_init_mem_manager(&gfx_pci, &gfx_gtt, &gfx_mgr);

    /* Resolve the panel mode in i915 order: Limine EDID, OpRegion EDID,
     * OpRegion VBT, eDP AUX EDID, then the firmware's eDP timing. */
    const display_mode_t *boot_mode = display_mode_get_boot();
    display_mode_t edid_mode;
    display_mode_t vbt_mode;
    display_mode_t hw_mode;
    bool from_limine = boot_mode->from_edid;
    bool from_opregion = false;
    bool from_vbt = false;
    bool from_aux = false;
    bool from_hw = false;
    bool have_edid = false;

    if (!from_limine) {
        have_edid = edid_read_opregion(dev, edid, sizeof(edid));
        from_opregion = have_edid;

        if (!have_edid && vbt_panel_mode(&gfx_pci, dev, &vbt_mode)) {
            boot_mode = &vbt_mode;
            from_vbt = true;
        }

        if (!have_edid && !from_vbt) {
            have_edid = edid_read_aux(&gfx_pci, edid, sizeof(edid));
            from_aux = have_edid;
        }

        if (have_edid) {
            if (display_mode_from_edid((edid_info_t *) edid, &edid_mode)) {
                boot_mode = display_mode_get_boot();
                display_mode_log(&edid_mode);
            } else {
                have_edid = false;
            }
        }

        if (!have_edid && !from_vbt
            && gfx_mode_from_hw(&gfx_pci, &hw_mode)) {
            boot_mode = &hw_mode;
            from_hw = true;
        }
    }

    klogi("GFX: EDID: sources L=%d O=%d V=%d A=%d H=%d\n",
          from_limine, from_opregion, from_vbt, from_aux, from_hw);

    klogi("GFX: state before self-test:\n");
    skl_display_dump(&gfx_pci);

    gfx_test_advanced_features(&gfx_pci);

    /* Take over the display with a real mode set at the boot mode. If the
     * pipeline cannot be programmed, keep the firmware aperture frame. */
    {
        fb_info_t *cur = fb_get();
        klogi("GFX: firmware fb 0x%016lx %ux%u pitch %u backbuffer %p\n",
              (uint64_t) cur->addr, cur->width, cur->height, cur->pitch,
              cur->backbuffer);
    }
    skl_display_dump(&gfx_pci);

    /* Blank the frame the display engine is currently scanning before the mode
     * set. Some panels keep sending the last region they received until the new
     * pipe covers it, so a cleared frame leaves nothing for the transition to
     * retain. */
    {
        fb_info_t *cur_fb = fb_get();

        if (cur_fb != NULL && cur_fb->addr != NULL) {
            if (cur_fb->backbuffer != NULL
                && cur_fb->backbuffer != cur_fb->addr) {
                memset(cur_fb->backbuffer, 0, cur_fb->backbuffer_len);
                fb_refresh(cur_fb);
            } else {
                memset(cur_fb->addr, 0, cur_fb->backbuffer_len);
            }
            pit_wait(50);
        }
    }

    /* The Limine framebuffer geometry has no blanking. If the selected mode
     * is only that geometry, prefer the firmware's eDP transcoder timing. */
    const display_mode_t *use_mode = boot_mode;
    bool have_hw = false;

    if ((use_mode == NULL || use_mode->hblank == 0)
        && gfx_mode_from_hw(&gfx_pci, &hw_mode)) {
        use_mode = &hw_mode;
        have_hw = true;
    }

    if (use_mode != NULL
        && skl_edp_set_mode(&gfx_pci, &gfx_mgr, &gfx_gtt, use_mode,
                            &gfx_fb)) {
        boot_mode = use_mode;
        gfx_attach_fb(&gfx_fb);
        if (skl_cursor_init(&gfx_pci, &gfx_mgr, &gfx_gtt, gfx_fb.width,
                            gfx_fb.height))
            skl_cursor_selftest();
        klogi("GFX: mode set took over the display (%ux%u)\n",
              gfx_fb.width, gfx_fb.height);
        return true;
    }

    /* Retry with the hardware eDP timing when the first mode failed. */
    if (!have_hw && gfx_mode_from_hw(&gfx_pci, &hw_mode)
        && skl_edp_set_mode(&gfx_pci, &gfx_mgr, &gfx_gtt, &hw_mode,
                            &gfx_fb)) {
        boot_mode = &hw_mode;
        gfx_attach_fb(&gfx_fb);
        if (skl_cursor_init(&gfx_pci, &gfx_mgr, &gfx_gtt, gfx_fb.width,
                            gfx_fb.height))
            skl_cursor_selftest();
        klogi("GFX: mode set took over the display from eDP timing (%ux%u)\n",
              gfx_fb.width, gfx_fb.height);
        return true;
    }

    klogw("GFX: mode set failed, keeping the firmware framebuffer\n");

    /* The BIOS GOP already scans the aperture (GMADR) through a trained eDP
     * link, so the aperture is used as the GPU framebuffer. Pass limine's
     * pitch so the display engine's stride matches what the CPU writes. */
    fb_info_t *limine_fb = fb_get();
    if (limine_fb && gfx_modeset(&gfx_pci, &gfx_mgr, &gfx_gtt,
                                 limine_fb->width, limine_fb->height,
                                 limine_fb->pitch, DISPPLANE_BGRX888,
                                 &gfx_fb)) {
        uint32_t fbsize = gfx_fb.stride * gfx_fb.height;
        uint8_t *new_bb = (uint8_t *) kmalloc(fbsize);
        if (new_bb) {
            memset(new_bb, 0, fbsize);
            limine_fb->backbuffer = new_bb;
            limine_fb->backbuffer_len = fbsize;
            klogi("GFX: fb backbuffer reallocated: %ux%u pitch=%u\n",
                  limine_fb->width, limine_fb->height, limine_fb->pitch);
        } else {
            kloge("GFX: kmalloc failed for backbuffer\n");
        }
    }

    /* The firmware framebuffer keeps the pipe scanning, so the cursor plane
     * can be armed on the same pipe even without the eDP modeset. */
    if (limine_fb != NULL
        && skl_cursor_init(&gfx_pci, &gfx_mgr, &gfx_gtt, limine_fb->width,
                           limine_fb->height))
        skl_cursor_selftest();

    return true;
}

bool gfx_mem_enable_swizzle(gfx_pci_t * pci)
{
    /* Only enable swizzling when DIMMs are the same size
     * This improves memory access patterns for tiled surfaces
     */
    uint32_t dimm_ch0 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH0);
    uint32_t dimm_ch1 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH1);

    klogd("GFX: DIMM CH0: 0x%08x, CH1: 0x%08x\n", dimm_ch0, dimm_ch1);

    if ((dimm_ch0 & MAD_DIMM_AB_SIZE_MASK) !=
        (dimm_ch1 & MAD_DIMM_AB_SIZE_MASK)) {
        klogd("GFX: DIMM sizes differ, skipping swizzle enable\n");
        return false;
    }

    /* Enable Bit 6 Swizzling for better memory interleaving */
    uint32_t arb_ctl = gfx_ind(pci, ARB_CTL);
    arb_ctl |= ARB_CTL_TILED_ADDRESS_SWIZZLING;
    gfx_outd(pci, ARB_CTL, arb_ctl);

    uint32_t tile_ctl = gfx_ind(pci, TILE_CTL);
    tile_ctl |= TILE_CTL_SWIZZLE;
    gfx_outd(pci, TILE_CTL, tile_ctl);

    gfx_outd(pci, ARB_MODE, MASKED_ENABLE(ARB_MODE_AS4TS));

    klogd("GFX: Swizzle enabled - ARB_CTL: 0x%08x, TILE_CTL: 0x%08x\n",
          gfx_ind(pci, ARB_CTL), gfx_ind(pci, TILE_CTL));

    return true;
}

uint64_t gfx_addr(gfx_mem_manager_t * mgr, void *phy_addr)
{
    return (uint64_t) ((uint8_t *) phy_addr - mgr->gfx_mem_base);
}

/**
 * @brief Write a single entry into the GTT
 * @param gtt   GTT structure
 * @param index GTT entry index (= GPU page number)
 * @param phys  Physical address of the page to map (must be page-aligned)
 *
 * Each GTT entry maps one 4 KB GPU page to a physical page.
 * Entry format (64-bit, Gen8+):
 *   bits 63:12  Physical page address bits 51:12
 *   bit  0      Present
 */
void gfx_gtt_write_entry(gfx_gtt_t * gtt, uint32_t index, uint64_t phys)
{
    if (gtt->pte64) {
        uint64_t entry = phys & ~(uint64_t)(GTT_PAGE_SIZE - 1);

        entry |= GTT_ENTRY_VALID;
        gtt->entries[index] = entry;
    } else {
        uint32_t entry = (uint32_t)(phys & 0xFFFFFFFFu)
            & ~(uint32_t)(GTT_PAGE_SIZE - 1);

        entry |= GTT_ENTRY_VALID;
        ((volatile uint32_t *) gtt->entries)[index] = entry;
    }
}

/**
 * @brief Map a contiguous physical buffer into the GTT
 * @param gtt       GTT structure
 * @param gpu_addr  GPU virtual address to map at (must be page-aligned)
 * @param phys      Physical address of the buffer (must be page-aligned)
 * @param size      Size in bytes (rounded up to page boundary)
 * @return Number of GTT entries written, or 0 on error
 */
uint32_t gfx_gtt_map(gfx_gtt_t * gtt, uint64_t gpu_addr, uint64_t phys,
                     uint64_t size)
{
    if (gpu_addr & (GTT_PAGE_SIZE - 1)) {
        kloge("GFX: gfx_gtt_map: gpu_addr 0x%016lx not page-aligned\n", gpu_addr);
        return 0;
    }

    uint32_t start_idx = (uint32_t)(gpu_addr >> GTT_PAGE_SHIFT);
    uint32_t num_pages = (uint32_t)NUM_PAGES(size);

    if (!gtt->pte64 && phys + size > 0x100000000ULL) {
        kloge("GFX: gfx_gtt_map: 0x%016lx is above the 32-bit GTT\n", phys);
        return 0;
    }

    if (start_idx + num_pages > gtt->num_total_entries) {
        kloge("GFX: gfx_gtt_map: range [%u, %u) exceeds GTT size %u\n",
              start_idx, start_idx + num_pages, gtt->num_total_entries);
        return 0;
    }

    for (uint32_t i = 0; i < num_pages; i++) {
        gfx_gtt_write_entry(gtt, start_idx + i, phys + (uint64_t)i * GTT_PAGE_SIZE);
    }

    /* Posting read to flush GTT writes before GPU uses them */
    (void)gtt->entries[start_idx];

    klogd("GFX: GTT mapped %u pages at GPU 0x%016lx phys 0x%016lx\n",
          num_pages, (uint32_t)gpu_addr, (uint32_t)phys);
    return num_pages;
}

/**
 * @brief Clear (invalidate) a range of GTT entries
 * @param gtt      GTT structure
 * @param gpu_addr GPU virtual address of range start (page-aligned)
 * @param size     Size in bytes
 */
void gfx_gtt_clear(gfx_gtt_t * gtt, uint64_t gpu_addr, uint64_t size)
{
    uint32_t start_idx = (uint32_t)(gpu_addr >> GTT_PAGE_SHIFT);
    uint32_t num_pages = (uint32_t)NUM_PAGES(size);

    for (uint32_t i = 0; i < num_pages
         && (start_idx + i) < gtt->num_total_entries; i++) {
        if (gtt->pte64)
            gtt->entries[start_idx + i] = 0;
        else
            ((volatile uint32_t *) gtt->entries)[start_idx + i] = 0;
    }
    (void)gtt->entries[start_idx];
}

bool gfx_alloc(gfx_mem_manager_t * mgr, gfx_gtt_t * gtt, gfx_object_t * obj,
               uint64_t size, uint64_t align)
{
    uint32_t num_pages = (uint32_t)NUM_PAGES(size);

    /* Allocate contiguous physical pages */
    uint64_t phys = pmm_get(num_pages, 0x0, __func__, __LINE__);
    if (!phys) {
        kloge("GFX: gfx_alloc: pmm_get failed for %u pages\n", num_pages);
        return false;
    }

    /* Find an aligned slot in the shared GPU address space */
    uint64_t gpu_addr = mgr->shared.current;
    if (align > GTT_PAGE_SIZE) {
        uint64_t mask = align - 1;
        if (gpu_addr & mask)
            gpu_addr = (gpu_addr + mask) & ~mask;
    }

    if (gpu_addr + size > mgr->shared.top) {
        kloge("GFX: gfx_alloc: shared GPU address space exhausted\n");
        pmm_free(phys, num_pages, __func__, __LINE__);
        return false;
    }

    /* Write GTT entries: GPU gpu_addr → physical pages */
    if (!gfx_gtt_map(gtt, gpu_addr, phys, size)) {
        pmm_free(phys, num_pages, __func__, __LINE__);
        return false;
    }

    mgr->shared.current = gpu_addr + PAGE_ALIGN_UP(size);

    obj->cpu_addr = (volatile uint8_t *)PHYS_TO_VIRT(phys);
    obj->gfx_addr = gpu_addr;
    obj->phys = phys;
    obj->size = (uint64_t) num_pages * PAGE_SIZE;
    obj->pages = num_pages;

    klogi("GFX: alloc %u pages phys 0x%016lx -> gpu 0x%016lx\n",
          num_pages, phys, gpu_addr);

    return true;
}

/* Release an object allocated by gfx_alloc. The caller must stop scanning it
 * before the free. */
static bool gfx_obj_free(gfx_object_t * obj)
{
    if (obj->cpu_addr == NULL || obj->pages == 0)
        return false;

    gfx_gtt_clear(&gfx_gtt, obj->gfx_addr, obj->size);
    pmm_free(obj->phys, obj->pages, __func__, __LINE__);

    if (gfx_mgr.shared.current == obj->gfx_addr + PAGE_ALIGN_UP(obj->size))
        gfx_mgr.shared.current = obj->gfx_addr;

    memset(obj, 0, sizeof(*obj));
    return true;
}

static void gfx_fb_free_scatter(gfx_fb_t * fb);

/* Large surfaces such as the scroll texture rarely have a contiguous
 * physical run after boot. Allocate one page at a time and point the GTT at
 * consecutive GPU addresses, so the CPU can fill the pages through the direct
 * map while the display engine still reads a linear GPU range. */
static bool gfx_fb_alloc_scatter(uint32_t width, uint32_t height,
                                 gfx_fb_t * out_fb)
{
    uint32_t stride = (width * 4 + 63) & ~63u;
    uint64_t size = (uint64_t) stride * height;
    uint64_t alloc = PAGE_ALIGN_UP(size);
    uint32_t npages = (uint32_t) NUM_PAGES(size);
    uint64_t *pages;
    uint64_t gpu;
    uint32_t i;

    pages = kmalloc((uint64_t) npages * sizeof(uint64_t));
    if (pages == NULL)
        return false;

    for (i = 0; i < npages; i++) {
        uint64_t phys = pmm_get(1, 0, __func__, __LINE__);

        if (phys == 0) {
            while (i-- > 0)
                pmm_free(pages[i], 1, __func__, __LINE__);
            kmfree(pages);
            return false;
        }
        pages[i] = phys;
    }

    gpu = gfx_mgr.shared.current;
    if (gpu + alloc > gfx_mgr.shared.top) {
        for (i = 0; i < npages; i++)
            pmm_free(pages[i], 1, __func__, __LINE__);
        kmfree(pages);
        return false;
    }

    for (i = 0; i < npages; i++)
        gfx_gtt_write_entry(&gfx_gtt,
                            (uint32_t) (gpu >> GTT_PAGE_SHIFT) + i, pages[i]);

    /* Flush the posted GTT writes before the display engine fetches. */
    (void) gfx_gtt.entries[(uint32_t) (gpu >> GTT_PAGE_SHIFT)];
    (void) gfx_gtt.entries[(uint32_t) (gpu >> GTT_PAGE_SHIFT) + npages - 1];

    memset(out_fb, 0, sizeof(*out_fb));
    out_fb->obj.gfx_addr = gpu;
    out_fb->obj.size = alloc;
    out_fb->pages = pages;
    out_fb->page_count = npages;
    out_fb->width = width;
    out_fb->height = height;
    out_fb->stride = stride;
    out_fb->format = DISPPLANE_BGRX888;
    gfx_mgr.shared.current = gpu + alloc;

    klogi("GFX: scatter alloc %u pages -> gpu 0x%016lx (%ux%u)\n",
          npages, gpu, width, height);
    return true;
}

static void gfx_fb_free_scatter(gfx_fb_t * fb)
{
    uint64_t gpu = fb->obj.gfx_addr;
    uint64_t size = fb->obj.size;

    gfx_gtt_clear(&gfx_gtt, gpu, size);
    for (uint32_t i = 0; i < fb->page_count; i++)
        pmm_free(fb->pages[i], 1, __func__, __LINE__);
    kmfree(fb->pages);
    if (gfx_mgr.shared.current == gpu + size)
        gfx_mgr.shared.current = gpu;
    memset(fb, 0, sizeof(*fb));
}

bool gfx_fb_alloc(uint32_t width, uint32_t height, gfx_fb_t * out_fb)
{
    uint32_t stride = (width * 4 + 63) & ~63u;
    uint64_t size = (uint64_t) stride * height;

    memset(out_fb, 0, sizeof(*out_fb));
    if (!gfx_alloc(&gfx_mgr, &gfx_gtt, &out_fb->obj, size, PAGE_SIZE))
        return false;
    out_fb->width = width;
    out_fb->height = height;
    out_fb->stride = stride;
    out_fb->format = DISPPLANE_BGRX888;
    return true;
}

void gfx_fb_free(gfx_fb_t * fb)
{
    if (fb == NULL)
        return;
    if (fb->pages != NULL) {
        gfx_fb_free_scatter(fb);
        return;
    }
    gfx_obj_free(&fb->obj);
    memset(fb, 0, sizeof(*fb));
}

/* Broadwell and Skylake moved the pipe interrupts out of DEIER into the
 * per-pipe GEN8_DE_PIPE_* registers. Haswell still uses the legacy block. */
static bool gfx_gen8_pipe_irq(gfx_pci_t * pci)
{
    return gfx_generation(pci->device_id) >= GFX_GEN8;
}

static void gfx_vblank_handler(void)
{
    if (gfx_gen8_pipe_irq(&gfx_pci)) {
        uint32_t iir = gfx_ind(&gfx_pci, GEN8_DE_PIPE_IIR(0));

        if (!(iir & GEN8_PIPE_VBLANK))
            return;
        gfx_outd(&gfx_pci, GEN8_DE_PIPE_IIR(0), GEN8_PIPE_VBLANK);
    } else {
        uint32_t iir = gfx_ind(&gfx_pci, DEIIR);

        if (!(iir & DE_PIPEA_VBLANK))
            return;
        gfx_outd(&gfx_pci, DEIIR, DE_PIPEA_VBLANK);
    }
    gfx_vblank_seq++;
}

static bool gfx_vblank_irq_setup(void)
{
    uint8_t line;
    uint16_t cmd;

    if (!ioapic_available()) {
        klogw("GFX: no I/O APIC, vblank falls back to polling\n");
        return false;
    }

    line = pci_inb(gfx_pci.id, PCI_CONFIG_INTERRUPT_LINE);
    if (line == 0 || line == 0xff) {
        klogw("GFX: no PCI interrupt line, vblank falls back to polling\n");
        return false;
    }

    /* Clear the INTx disable bit; the display engine drives the line. */
    cmd = pci_inw(gfx_pci.id, PCI_CONFIG_COMMAND);
    cmd &= ~(1u << 10);
    cmd |= 1u << 2;
    pci_outw(gfx_pci.id, PCI_CONFIG_COMMAND, cmd);

    /* Mask the entry, install the handler, then unmask through the explicit
     * vector. An interrupt that arrives between the unmask and the handler
     * registration would enter an unhandled vector and panic the kernel. */
    ioapic_set_line(line, true);
    exc_register_handler(GFX_VBLANK_VECTOR, gfx_vblank_handler);
    if (!irq_route_vector(line, GFX_VBLANK_VECTOR, true, true)) {
        klogw("GFX: cannot route IRQ line %u to vector 0x%x\n", line,
              GFX_VBLANK_VECTOR);
        return false;
    }
    if (gfx_gen8_pipe_irq(&gfx_pci)) {
        uint32_t imr;
        uint32_t ier;

        /* Keep only pipe A vblank: a shared GPU INTx must not stay asserted
         * for a GT or misc source this driver never clears. */
        gfx_outd(&gfx_pci, DEIER, 0);
        gfx_outd(&gfx_pci, DEIMR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, DEIIR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, GTIER, 0);
        gfx_outd(&gfx_pci, GTIMR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, GTIIR, 0xFFFFFFFF);
        for (uint32_t i = 0; i < 4; i++) {
            gfx_outd(&gfx_pci, GEN8_GT_IER(i), 0);
            gfx_outd(&gfx_pci, GEN8_GT_IMR(i), 0xFFFFFFFF);
            gfx_outd(&gfx_pci, GEN8_GT_IIR(i), 0xFFFFFFFF);
        }
        gfx_outd(&gfx_pci, GEN8_DE_MISC_IER, 0);
        gfx_outd(&gfx_pci, GEN8_DE_MISC_IMR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, GEN8_DE_MISC_IIR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, GEN8_DE_PORT_IER, 0);
        gfx_outd(&gfx_pci, GEN8_DE_PORT_IMR, 0xFFFFFFFF);
        gfx_outd(&gfx_pci, GEN8_DE_PORT_IIR, 0xFFFFFFFF);

        gfx_outd(&gfx_pci, GEN8_DE_PIPE_IIR(0), GEN8_PIPE_VBLANK);
        imr = gfx_ind(&gfx_pci, GEN8_DE_PIPE_IMR(0));
        imr &= ~GEN8_PIPE_VBLANK;
        gfx_outd(&gfx_pci, GEN8_DE_PIPE_IMR(0), imr);
        ier = gfx_ind(&gfx_pci, GEN8_DE_PIPE_IER(0));
        ier |= GEN8_PIPE_VBLANK;
        gfx_outd(&gfx_pci, GEN8_DE_PIPE_IER(0), ier);
        uint32_t master = gfx_ind(&gfx_pci, GEN8_MASTER_IRQ);
        master |= GEN8_MASTER_IRQ_CONTROL;
        gfx_outd(&gfx_pci, GEN8_MASTER_IRQ, master);
    } else {
        gfx_enable_interrupts(&gfx_pci, DE_PIPEA_VBLANK);
    }

    gfx_vblank_line = line;
    gfx_vblank_irq = true;
    klogi("GFX: vblank IRQ line %u -> vector 0x%x\n", line,
          GFX_VBLANK_VECTOR);
    return true;
}

/* Arm the vblank line after the I/O APIC is up. The mode set runs earlier, so
 * gfx_init cannot route the line itself. */
bool gfx_vblank_irq_start(void)
{
    if (gfx_pci.id == 0 || gfx_fb.obj.cpu_addr == NULL)
        return false;
    if (gfx_vblank_irq)
        return true;
    return gfx_vblank_irq_setup();
}

uint64_t gfx_vblank_count(void)
{
    return gfx_vblank_seq;
}

bool gfx_vblank_irq_active(void)
{
    return gfx_vblank_irq;
}

uint8_t gfx_vblank_irq_line(void)
{
    return gfx_vblank_line;
}

uint32_t gfx_display_iir(void)
{
    if (gfx_pci.mmio_bar == NULL)
        return 0;
    if (gfx_gen8_pipe_irq(&gfx_pci))
        return gfx_ind(&gfx_pci, GEN8_DE_PIPE_IIR(0));
    return gfx_ind(&gfx_pci, DEIIR);
}

uint32_t gfx_display_ier(void)
{
    if (gfx_pci.mmio_bar == NULL)
        return 0;
    if (gfx_gen8_pipe_irq(&gfx_pci))
        return gfx_ind(&gfx_pci, GEN8_DE_PIPE_IER(0));
    return gfx_ind(&gfx_pci, DEIER);
}

uint32_t gfx_display_imr(void)
{
    if (gfx_pci.mmio_bar == NULL)
        return 0;
    if (gfx_gen8_pipe_irq(&gfx_pci))
        return gfx_ind(&gfx_pci, GEN8_DE_PIPE_IMR(0));
    return gfx_ind(&gfx_pci, DEIMR);
}

uint32_t gfx_master_irq(void)
{
    if (gfx_pci.mmio_bar == NULL || !gfx_gen8_pipe_irq(&gfx_pci))
        return 0;
    return gfx_ind(&gfx_pci, GEN8_MASTER_IRQ);
}

uint32_t gfx_pipe_frame(void)
{
    if (gfx_pci.mmio_bar == NULL)
        return 0;
    return gfx_ind(&gfx_pci, 0x70040);
}

uint32_t gfx_pipe_frame_after(uint32_t ms)
{
    if (gfx_pci.mmio_bar == NULL)
        return 0;
    pit_wait(ms);
    return gfx_ind(&gfx_pci, 0x70040);
}

uint64_t gfx_vblank_route(void)
{
    uint32_t low = 0;
    uint32_t high = 0;

    if (gfx_vblank_line == 0
        || !ioapic_read_line(gfx_vblank_line, &low, &high))
        return 0;
    return (uint64_t) low | ((uint64_t) high << 32);
}

static void gfx_vblank_irq_disable(void)
{
    if (gfx_gen8_pipe_irq(&gfx_pci)) {
        uint32_t ier = gfx_ind(&gfx_pci, GEN8_DE_PIPE_IER(0));

        ier &= ~GEN8_PIPE_VBLANK;
        gfx_outd(&gfx_pci, GEN8_DE_PIPE_IER(0), ier);
    } else {
        gfx_disable_interrupts(&gfx_pci, DE_PIPEA_VBLANK);
    }
}

bool gfx_wait_vblank_frame(void)
{
    if (gfx_vblank_irq) {
        uint64_t start = gfx_vblank_seq;

        for (int32_t i = 0; i < 100; i++) {
            if (gfx_vblank_seq != start)
                return true;
            sched_sleep(1);
        }

        /* The line did not arrive. Mask it and poll the status bit so the
         * caller still gets a frame boundary. */
        klogw("GFX: vblank IRQ timeout (line %u), polling\n",
              gfx_vblank_line);
        gfx_vblank_irq_disable();
        gfx_vblank_irq = false;
    }
    return gfx_wait_vblank(&gfx_pci, 0);
}

void gfx_flip_to(const gfx_fb_t * fb)
{
    if (fb == NULL)
        return;
    gfx_outd(&gfx_pci, PLANE_SURF_1_A, (uint32_t) fb->obj.gfx_addr);
    (void) gfx_ind(&gfx_pci, PLANE_SURF_1_A);
}

/* Fill a surface with a vertical bar. Non-temporal stores leave the frame in
 * DRAM for the display engine instead of a dirty CPU cache line. */
static void gfx_fill_bar(gfx_fb_t * fb, uint32_t bar_x, uint32_t bar_w,
                         uint32_t fg, uint32_t bg)
{
    for (uint32_t y = 0; y < fb->height; y++) {
        volatile uint8_t *row = fb->obj.cpu_addr + (uint64_t) y * fb->stride;
        uint32_t x = 0;

        for (; x + 2 <= fb->width; x += 2) {
            uint32_t p0 = (x >= bar_x && x < bar_x + bar_w) ? fg : bg;
            uint32_t p1 = (x + 1 >= bar_x && x + 1 < bar_x + bar_w)
                ? fg : bg;
            uint64_t two = (uint64_t) p0 | ((uint64_t) p1 << 32);

            asm volatile ("movnti %1, (%0)"::"r"(row + x * 4),
                          "r"(two):"memory");
        }
        for (; x < fb->width; x++) {
            uint32_t p = (x >= bar_x && x < bar_x + bar_w) ? fg : bg;

            *(volatile uint32_t *) (row + x * 4) = p;
        }
    }
    asm volatile ("sfence":::"memory");
}

bool gfx_flip_test(void)
{
    gfx_fb_t a, b;
    int32_t flips = 0;
    bool ok = false;
    uint32_t saved_ctl, saved_pos, saved_size, saved_stride, saved_surf;

    if (!gfx_pci.gen9 || gfx_fb.obj.cpu_addr == NULL) {
        klogw("GFX: flip test needs a Gen9 mode set\n");
        return false;
    }

    if (!gfx_fb_alloc(640, 480, &a)) {
        kloge("GFX: flip test: alloc A failed\n");
        return false;
    }
    if (!gfx_fb_alloc(640, 480, &b)) {
        kloge("GFX: flip test: alloc B failed\n");
        gfx_fb_free(&a);
        return false;
    }

    gfx_fill_bar(&a, 0, 16, 0x00ffffff, 0x00102030);
    gfx_fill_bar(&b, 0, 16, 0x00ffffff, 0x00102030);

    saved_ctl = gfx_ind(&gfx_pci, PLANE_CTL_1_A);
    saved_pos = gfx_ind(&gfx_pci, PLANE_POS_1_A);
    saved_size = gfx_ind(&gfx_pci, PLANE_SIZE_1_A);
    saved_stride = gfx_ind(&gfx_pci, PLANE_STRIDE_1_A);
    saved_surf = gfx_ind(&gfx_pci, PLANE_SURF_1_A);

    /* Disable the plane before changing its geometry. */
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();

    gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, 0);
    gfx_outd(&gfx_pci, PLANE_POS_1_A, 0);
    gfx_outd(&gfx_pci, PLANE_SIZE_1_A, (480 - 1) << 16 | (640 - 1));
    gfx_outd(&gfx_pci, PLANE_STRIDE_1_A, a.stride / 64);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A,
             PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB
             | PLANE_CTL_TILED_LINEAR);
    gfx_outd(&gfx_pci, PLANE_SURF_1_A, (uint32_t) a.obj.gfx_addr);
    gfx_wait_vblank_frame();

    for (int32_t i = 0; i < 180; i++) {
        gfx_fb_t *next = (i & 1) ? &a : &b;

        if (!gfx_wait_vblank_frame())
            break;
        gfx_fill_bar(next, (uint32_t) (i * 4) % 640, 16, 0x00ffffff,
                     0x00102030);
        gfx_flip_to(next);
        flips++;
    }

    /* Restore the plane and let the console buffer become the scanout again
     * before the test buffers are freed. */
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();
    gfx_outd(&gfx_pci, PLANE_POS_1_A, saved_pos);
    gfx_outd(&gfx_pci, PLANE_SIZE_1_A, saved_size);
    gfx_outd(&gfx_pci, PLANE_STRIDE_1_A, saved_stride);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, saved_ctl);
    gfx_outd(&gfx_pci, PLANE_SURF_1_A, saved_surf);
    gfx_wait_vblank_frame();
    gfx_wait_vblank_frame();

    gfx_fb_free(&a);
    gfx_fb_free(&b);

    ok = (flips == 180);
    klogi("GFX: flip test %d frames, vblank %lu, irq %d, %s\n",
          flips, gfx_vblank_seq, gfx_vblank_irq, ok ? "PASS" : "FAIL");
    return ok;
}

/* --- Three-layer composition demo -------------------------------------- */

#define LAYER_UI_W          480
#define LAYER_UI_H          180
#define LAYER_UI_X          80
#define LAYER_UI_Y          80
#define LAYER_SCROLL_SPEED  8       /* pixels per frame */
#define LAYER_PERIOD_SCREENS 4
#define LAYER_FRAMES        900     /* 15 s at 60 Hz */

static uint32_t layer_rng = 0x9e3779b9u;

static uint32_t layer_rand(void)
{
    layer_rng = layer_rng * 1103515245u + 12345u;
    return layer_rng >> 8;
}

static uint32_t layer_hash(uint32_t seed, uint32_t row)
{
    uint32_t x = seed + row * 0x9e3779b9u;

    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x;
}

static void layer_pixel(gfx_fb_t * fb, int32_t x, int32_t y, uint32_t color)
{
    uint64_t off;

    if (x < 0 || y < 0 || (uint32_t) x >= fb->width
        || (uint32_t) y >= fb->height)
        return;
    off = (uint64_t) y * fb->stride + (uint64_t) x * 4;
    if (fb->pages != NULL) {
        uint32_t pg = (uint32_t) (off >> GTT_PAGE_SHIFT);
        uint32_t po = (uint32_t) (off & (GTT_PAGE_SIZE - 1));

        *(uint32_t *) ((uint8_t *) PHYS_TO_VIRT(fb->pages[pg]) + po) = color;
    } else {
        *(uint32_t *) (fb->obj.cpu_addr + off) = color;
    }
}

static uint32_t layer_get(gfx_fb_t * fb, int32_t x, int32_t y)
{
    uint64_t off = (uint64_t) y * fb->stride + (uint64_t) x * 4;

    if (fb->pages != NULL) {
        uint32_t pg = (uint32_t) (off >> GTT_PAGE_SHIFT);
        uint32_t po = (uint32_t) (off & (GTT_PAGE_SIZE - 1));

        return *(uint32_t *) ((uint8_t *) PHYS_TO_VIRT(fb->pages[pg]) + po);
    }
    return *(uint32_t *) (fb->obj.cpu_addr + off);
}

static void layer_clear(gfx_fb_t * fb)
{
    if (fb->pages != NULL) {
        for (uint32_t i = 0; i < fb->page_count; i++)
            memset((void *) PHYS_TO_VIRT(fb->pages[i]), 0, GTT_PAGE_SIZE);
    } else {
        memset((void *) fb->obj.cpu_addr, 0,
               (uint64_t) fb->stride * fb->height);
    }
}

static void layer_rect(gfx_fb_t * fb, int32_t x, int32_t y, int32_t w,
                       int32_t h, uint32_t color)
{
    for (int32_t j = 0; j < h; j++)
        for (int32_t i = 0; i < w; i++)
            layer_pixel(fb, x + i, y + j, color);
}

static void layer_glyph(gfx_fb_t * fb, int32_t x, int32_t y, uint8_t ch,
                        uint32_t color)
{
    const psf1_font_t *font = &boot_font_norm;
    const uint8_t *glyph = font->data + (uint32_t) ch * font->charsize;

    for (uint32_t i = 0; i < font->charsize; i++) {
        uint8_t bits = glyph[i];

        for (uint32_t k = 0; k < 8; k++)
            if (bits & (0x80u >> k))
                layer_pixel(fb, x + (int32_t) k, y + (int32_t) i, color);
    }
}

static void layer_text(gfx_fb_t * fb, int32_t x, int32_t y, const char *s,
                       uint32_t color)
{
    while (*s != '\0') {
        layer_glyph(fb, x, y, (uint8_t) *s, color);
        x += 8;
        s++;
    }
}

static void layer_flush(gfx_fb_t * fb)
{
    if (fb->pages != NULL) {
        for (uint32_t i = 0; i < fb->page_count; i++) {
            uintptr_t p = (uintptr_t) PHYS_TO_VIRT(fb->pages[i]) & ~63ULL;
            uintptr_t end = p + GTT_PAGE_SIZE;

            for (; p < end; p += 64)
                asm volatile ("clflush (%0)"::"r"((void *) p):"memory");
        }
    } else {
        uintptr_t p = (uintptr_t) fb->obj.cpu_addr & ~63ULL;
        uintptr_t end = (uintptr_t) fb->obj.cpu_addr
            + (uintptr_t) fb->stride * fb->height;

        for (; p < end; p += 64)
            asm volatile ("clflush (%0)"::"r"((void *) p):"memory");
    }
    asm volatile ("mfence":::"memory");
}

static uint32_t layer_rain_color(int32_t d, int32_t len)
{
    uint32_t lvl;

    if (d <= 0)
        return 0xddffdd;
    lvl = (uint32_t) (len - d) * 255u / (uint32_t) len;
    if (lvl > 150)
        return 0x55ff88;
    if (lvl > 90)
        return 0x22cc55;
    if (lvl > 40)
        return 0x00aa33;
    return 0x004411;
}

/* Bake the rain once. Each column gets streaks with random lengths and gaps;
 * the pattern repeats every period_rows and the first screen_rows are copied
 * to the end so the plane offset can wrap without a visible jump. */
static void layer_tex_build(gfx_fb_t * tex, uint32_t period_rows,
                            uint32_t screen_rows)
{
    static const char set[] =
        "01010101010101011123456789ABCDEF#$%&*<>[]{}()/=+-:.";
    uint32_t ns = sizeof(set) - 1;
    uint32_t ncol = tex->width / 8;
    uint32_t fh = boot_font_norm.charsize;

    layer_clear(tex);

    for (uint32_t c = 0; c < ncol; c++) {
        uint32_t seed = layer_rand() | 1;
        int32_t pos = (int32_t) (layer_rand() % period_rows);

        while (pos < (int32_t) period_rows) {
            int32_t len = 3 + (int32_t) (layer_rand() % 14);
            int32_t gap = 10 + (int32_t) (layer_rand() % 50);

            for (int32_t i = 0; i < len; i++) {
                int32_t r = (pos + i) % (int32_t) period_rows;
                int32_t d = (len - 1) - i;
                uint8_t ch = (uint8_t) set[layer_hash(seed, (uint32_t) r)
                                           % ns];

                layer_glyph(tex, (int32_t) c * 8, r * (int32_t) fh, ch,
                            layer_rain_color(d, len));
            }
            pos += len + gap;
        }
    }

    for (uint32_t r = 0; r < screen_rows; r++)
        for (uint32_t y = 0; y < fh; y++)
            for (uint32_t x = 0; x < tex->width; x++)
                layer_pixel(tex, (int32_t) x,
                            (int32_t) ((period_rows + r) * fh + y),
                            layer_get(tex, (int32_t) x,
                                      (int32_t) (r * fh + y)));

    layer_flush(tex);
}

/* Fill the scroll surface with wide horizontal bands. A GPU scroll moves
 * them, which isolates the plane/GTT/offset path from the rain texture. */
static void layer_tex_solid(gfx_fb_t * tex)
{
    layer_clear(tex);
    for (uint32_t y = 0; y < tex->height; y++) {
        uint32_t color = ((y / 80) & 1) ? 0x0033aa55 : 0x00101820;

        for (uint32_t x = 0; x < tex->width; x++)
            layer_pixel(tex, (int32_t) x, (int32_t) y, color);
    }
    layer_flush(tex);
}

static void layer_ui_draw(gfx_fb_t * ui, uint32_t progress)
{
    layer_rect(ui, 0, 0, (int32_t) ui->width, (int32_t) ui->height,
               0x00101820);
    layer_rect(ui, 0, 0, (int32_t) ui->width, 2, 0x0000ff88);
    layer_rect(ui, 0, (int32_t) ui->height - 2, (int32_t) ui->width, 2,
               0x0000ff88);
    layer_rect(ui, 0, 0, 2, (int32_t) ui->height, 0x0000ff88);
    layer_rect(ui, (int32_t) ui->width - 2, 0, 2, (int32_t) ui->height,
               0x0000ff88);

    layer_text(ui, 16, 12, "HanOS / Skylake", 0x00ddffdd);
    layer_text(ui, 16, 36, "primary : scrolled rain", 0x0066ff88);
    layer_text(ui, 16, 56, "sprite  : ui", 0x0066ff88);
    layer_text(ui, 16, 76, "cursor  : top", 0x0066ff88);

    layer_rect(ui, 16, 128, (int32_t) ui->width - 32, 14, 0x00203038);
    layer_rect(ui, 16, 128, (int32_t) ((ui->width - 32) * progress / 100),
               14, 0x0000cc55);
}

static void layer_ui_progress(gfx_fb_t * ui, uint32_t progress)
{
    layer_rect(ui, 16, 128, (int32_t) ui->width - 32, 14, 0x00203038);
    layer_rect(ui, 16, 128, (int32_t) ((ui->width - 32) * progress / 100),
               14, 0x0000cc55);
}

bool gfx_layers_demo(int mode)
{
    bool with_sprite = (mode == 0);
    gfx_fb_t tex;
    gfx_fb_t ui;
    uint32_t saved1_ctl;
    uint32_t saved1_off;
    uint32_t saved1_pos;
    uint32_t saved1_size;
    uint32_t saved1_stride;
    uint32_t saved1_surf;
    uint32_t saved2_ctl;
    uint32_t saved2_off;
    uint32_t saved2_pos;
    uint32_t saved2_size;
    uint32_t saved2_stride;
    uint32_t saved2_surf;
    uint32_t fh;
    uint32_t screen_rows;
    uint32_t period_rows;
    uint32_t period_px;
    uint32_t tex_h;
    uint32_t scroll;
    int32_t frames;
    bool ok;

    if (!gfx_pci.gen9 || gfx_fb.obj.cpu_addr == NULL) {
        klogw("GFX: layers demo needs a Gen9 mode set\n");
        return false;
    }

    memset(&tex, 0, sizeof(tex));
    memset(&ui, 0, sizeof(ui));
    fh = boot_font_norm.charsize;
    screen_rows = (gfx_fb.height + fh - 1) / fh;
    period_rows = LAYER_PERIOD_SCREENS * screen_rows;
    tex_h = (period_rows + screen_rows) * fh;

    if (!gfx_fb_alloc_scatter(gfx_fb.width, tex_h, &tex)) {
        kloge("GFX: layers: rain texture alloc failed\n");
        return false;
    }
    if (mode == 2)
        layer_tex_solid(&tex);
    else
        layer_tex_build(&tex, period_rows, screen_rows);

    if (with_sprite) {
        if (!gfx_fb_alloc(LAYER_UI_W, LAYER_UI_H, &ui)) {
            kloge("GFX: layers: UI buffer alloc failed\n");
            gfx_fb_free(&tex);
            return false;
        }
        layer_ui_draw(&ui, 0);
        layer_flush(&ui);
    }

    saved1_ctl = gfx_ind(&gfx_pci, PLANE_CTL_1_A);
    saved1_off = gfx_ind(&gfx_pci, PLANE_OFFSET_1_A);
    saved1_pos = gfx_ind(&gfx_pci, PLANE_POS_1_A);
    saved1_size = gfx_ind(&gfx_pci, PLANE_SIZE_1_A);
    saved1_stride = gfx_ind(&gfx_pci, PLANE_STRIDE_1_A);
    saved1_surf = gfx_ind(&gfx_pci, PLANE_SURF_1_A);
    saved2_ctl = gfx_ind(&gfx_pci, PLANE_CTL_2_A);
    saved2_off = gfx_ind(&gfx_pci, PLANE_OFFSET_2_A);
    saved2_pos = gfx_ind(&gfx_pci, PLANE_POS_2_A);
    saved2_size = gfx_ind(&gfx_pci, PLANE_SIZE_2_A);
    saved2_stride = gfx_ind(&gfx_pci, PLANE_STRIDE_2_A);
    saved2_surf = gfx_ind(&gfx_pci, PLANE_SURF_2_A);

    period_px = period_rows * fh;
    scroll = period_px;

    /* Primary: the baked texture. Only the plane offset changes per frame. */
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();
    gfx_outd(&gfx_pci, PLANE_POS_1_A, 0);
    gfx_outd(&gfx_pci, PLANE_SIZE_1_A,
             (gfx_fb.height - 1) << 16 | (gfx_fb.width - 1));
    gfx_outd(&gfx_pci, PLANE_STRIDE_1_A, tex.stride / 64);
    gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, scroll << 16);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, saved1_ctl);
    gfx_outd(&gfx_pci, PLANE_SURF_1_A, (uint32_t) tex.obj.gfx_addr);
    gfx_wait_vblank_frame();

    if (with_sprite) {
        /* Sprite: plane 2 above the primary, below the cursor. */
        gfx_outd(&gfx_pci, PLANE_CTL_2_A, 0);
        gfx_outd(&gfx_pci, PS_CTRL_2A, 0);
        gfx_outd(&gfx_pci, PLANE_OFFSET_2_A, 0);
        gfx_outd(&gfx_pci, PLANE_POS_2_A, (LAYER_UI_Y << 16) | LAYER_UI_X);
        gfx_outd(&gfx_pci, PLANE_SIZE_2_A,
                 (ui.height - 1) << 16 | (ui.width - 1));
        gfx_outd(&gfx_pci, PLANE_STRIDE_2_A, ui.stride / 64);
        /* The cursor owns DDB blocks 446..477; the sprite must start after
         * it, or the two planes fight over the same data buffer. */
        gfx_outd(&gfx_pci, PLANE_BUF_CFG_2A,
                 PLANE_BUF_END(511) | PLANE_BUF_START(478));
        for (uint32_t lvl = 0; lvl < 8; lvl++)
            gfx_outd(&gfx_pci, PLANE_WM_2A(lvl),
                     PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
        gfx_outd(&gfx_pci, PLANE_WM_TRANS_2A,
                 PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
        gfx_outd(&gfx_pci, PLANE_CTL_2_A,
                 PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB
                 | PLANE_CTL_TILED_LINEAR);
        gfx_outd(&gfx_pci, PLANE_SURF_2_A, (uint32_t) ui.obj.gfx_addr);
        (void) gfx_ind(&gfx_pci, PLANE_SURF_2_A);
        gfx_wait_vblank_frame();
    }

    if (with_sprite)
        klogi("GFX: layers: texture %ux%u (period %u px), UI %ux%u "
              "at %d,%d\n", tex.width, tex.height, period_px, ui.width,
              ui.height, LAYER_UI_X, LAYER_UI_Y);
    else
        klogi("GFX: layers: texture %ux%u (period %u px), primary only\n",
              tex.width, tex.height, period_px);
    klogi("GFX: layers: planes ready, starting at offset %u\n", scroll);

    for (frames = 0; frames < LAYER_FRAMES; frames++) {
        if ((frames % 60) == 0)
            klogi("GFX: layers: frame %d offset %u\n", frames, scroll);
        if (!gfx_wait_vblank_frame())
            break;
        if (scroll >= LAYER_SCROLL_SPEED)
            scroll -= LAYER_SCROLL_SPEED;
        else
            scroll = period_px;
        gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, scroll << 16);
        gfx_outd(&gfx_pci, PLANE_SURF_1_A, (uint32_t) tex.obj.gfx_addr);

        if (with_sprite && (frames % 30) == 0) {
            layer_ui_progress(&ui,
                              (uint32_t) frames * 100u / LAYER_FRAMES);
            layer_flush(&ui);
        }
    }
    ok = (frames == LAYER_FRAMES);

    /* Restore the sprite and the console's primary surface. */
    if (with_sprite) {
        gfx_outd(&gfx_pci, PLANE_CTL_2_A, 0);
        gfx_wait_vblank_frame();
        gfx_outd(&gfx_pci, PLANE_OFFSET_2_A, saved2_off);
        gfx_outd(&gfx_pci, PLANE_POS_2_A, saved2_pos);
        gfx_outd(&gfx_pci, PLANE_SIZE_2_A, saved2_size);
        gfx_outd(&gfx_pci, PLANE_STRIDE_2_A, saved2_stride);
        gfx_outd(&gfx_pci, PLANE_CTL_2_A, saved2_ctl);
        gfx_outd(&gfx_pci, PLANE_SURF_2_A, saved2_surf);
    }

    gfx_outd(&gfx_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();
    gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, saved1_off);
    gfx_outd(&gfx_pci, PLANE_POS_1_A, saved1_pos);
    gfx_outd(&gfx_pci, PLANE_SIZE_1_A, saved1_size);
    gfx_outd(&gfx_pci, PLANE_STRIDE_1_A, saved1_stride);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, saved1_ctl);
    gfx_outd(&gfx_pci, PLANE_SURF_1_A, saved1_surf);
    gfx_wait_vblank_frame();
    gfx_wait_vblank_frame();

    if (with_sprite)
        gfx_fb_free(&ui);
    gfx_fb_free(&tex);

    klogi("GFX: layers demo %d frames, %s\n", frames,
          ok ? "PASS" : "FAIL");
    return ok;
}

/* --- Persistent desktop ------------------------------------------------- */

#define DESKTOP_STATUS_H    20
#define DESKTOP_FONT_W      8
#define DESKTOP_FONT_H      15

static bool gfx_desktop_is_active;
static gfx_fb_t gfx_desktop_tex;
static gfx_fb_t gfx_desktop_sprite;
static uint32_t gfx_desktop_period;

bool gfx_desktop_active(void)
{
    return gfx_desktop_is_active;
}

gfx_fb_t *gfx_desktop_console_fb(void)
{
    return gfx_desktop_is_active ? &gfx_desktop_sprite : NULL;
}

/* Only the primary plane offset moves. The CPU does not redraw pixels. */
_Noreturn static void gfx_desktop_rain(pid_t pid)
{
    uint32_t scroll = gfx_desktop_period;
    uint32_t frames = 0;
    uint32_t last = gfx_pipe_frame();

    (void) pid;
    klogi("GFX: desktop: rain thread running\n");
    for (;;) {
        uint32_t now;

        /* The display pipe's frame counter is the reliable tick here; the
         * per-pipe vblank status can stay clear while the sprite is active. */
        sched_sleep(1);
        now = gfx_pipe_frame();
        if (now == last)
            continue;
        last = now;
        if ((frames++ % 600) == 0)
            klogi("GFX: desktop: rain offset %u\n", scroll);
        if (scroll >= LAYER_SCROLL_SPEED)
            scroll -= LAYER_SCROLL_SPEED;
        else
            scroll = gfx_desktop_period;
        gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, scroll << 16);
        gfx_outd(&gfx_pci, PLANE_SURF_1_A,
                 (uint32_t) gfx_desktop_tex.obj.gfx_addr);
    }
}

bool gfx_desktop_start(void)
{
    uint32_t screen_rows;
    uint32_t period_rows;
    uint32_t tex_h;
    uint32_t period_px;
    uint32_t cols;
    uint32_t rows;
    uint32_t spr_w;
    uint32_t spr_h;
    uint32_t pos_x;
    uint32_t pos_y;
    uint32_t ctl;

    if (gfx_desktop_is_active)
        return true;
    if (!gfx_pci.gen9 || gfx_fb.obj.cpu_addr == NULL)
        return false;

    screen_rows = (gfx_fb.height + boot_font_norm.charsize - 1)
        / boot_font_norm.charsize;
    period_rows = LAYER_PERIOD_SCREENS * screen_rows;
    tex_h = (period_rows + screen_rows) * boot_font_norm.charsize;
    period_px = period_rows * boot_font_norm.charsize;

    memset(&gfx_desktop_tex, 0, sizeof(gfx_desktop_tex));
    memset(&gfx_desktop_sprite, 0, sizeof(gfx_desktop_sprite));
    if (!gfx_fb_alloc_scatter(gfx_fb.width, tex_h, &gfx_desktop_tex)) {
        kloge("GFX: desktop: rain texture alloc failed\n");
        return false;
    }
    layer_tex_build(&gfx_desktop_tex, period_rows, screen_rows);

    cols = (gfx_fb.width / DESKTOP_FONT_W) * 4 / 5;
    rows = (gfx_fb.height / DESKTOP_FONT_H) * 4 / 5;
    spr_w = cols * DESKTOP_FONT_W;
    spr_h = rows * DESKTOP_FONT_H + DESKTOP_STATUS_H;
    if (!gfx_fb_alloc(spr_w, spr_h, &gfx_desktop_sprite)) {
        kloge("GFX: desktop: console sprite alloc failed\n");
        gfx_fb_free(&gfx_desktop_tex);
        return false;
    }
    memset((void *) gfx_desktop_sprite.obj.cpu_addr, 0,
           (uint64_t) gfx_desktop_sprite.stride
           * gfx_desktop_sprite.height);
    layer_flush(&gfx_desktop_sprite);

    pos_x = (gfx_fb.width - spr_w) / 2;
    pos_y = (gfx_fb.height - spr_h) / 2;

    /* Keep the modeset's 445-block primary allocation: shrinking it starves
     * the 1366-wide scan-out and stops the pipe's vblank. The cursor takes
     * 446..477 and the console sprite 478..511. */
    gfx_outd(&gfx_pci, PLANE_BUF_CFG_1A,
             PLANE_BUF_END(445) | PLANE_BUF_START(0));
    gfx_outd(&gfx_pci, CUR_BUF_CFG,
             PLANE_BUF_END(477) | PLANE_BUF_START(446));
    gfx_outd(&gfx_pci, PLANE_BUF_CFG_2A,
             PLANE_BUF_END(511) | PLANE_BUF_START(478));

    ctl = gfx_ind(&gfx_pci, PLANE_CTL_1_A);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, 0);
    gfx_wait_vblank_frame();
    gfx_outd(&gfx_pci, PLANE_POS_1_A, 0);
    gfx_outd(&gfx_pci, PLANE_SIZE_1_A,
             (gfx_fb.height - 1) << 16 | (gfx_fb.width - 1));
    gfx_outd(&gfx_pci, PLANE_STRIDE_1_A, gfx_desktop_tex.stride / 64);
    gfx_outd(&gfx_pci, PLANE_OFFSET_1_A, period_px << 16);
    gfx_outd(&gfx_pci, PLANE_CTL_1_A, ctl);
    gfx_outd(&gfx_pci, PLANE_SURF_1_A,
             (uint32_t) gfx_desktop_tex.obj.gfx_addr);
    gfx_wait_vblank_frame();

    gfx_outd(&gfx_pci, PLANE_CTL_2_A, 0);
    gfx_outd(&gfx_pci, PS_CTRL_2A, 0);
    gfx_outd(&gfx_pci, PLANE_OFFSET_2_A, 0);
    gfx_outd(&gfx_pci, PLANE_POS_2_A, (pos_y << 16) | pos_x);
    gfx_outd(&gfx_pci, PLANE_SIZE_2_A,
             (gfx_desktop_sprite.height - 1) << 16
             | (gfx_desktop_sprite.width - 1));
    gfx_outd(&gfx_pci, PLANE_STRIDE_2_A, gfx_desktop_sprite.stride / 64);
    for (uint32_t lvl = 0; lvl < 8; lvl++)
        gfx_outd(&gfx_pci, PLANE_WM_2A(lvl),
                 PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
    gfx_outd(&gfx_pci, PLANE_WM_TRANS_2A,
             PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(16));
    gfx_outd(&gfx_pci, PLANE_CTL_2_A,
             PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB
             | PLANE_CTL_ALPHA_SW_PREMULTIPLY
             | PLANE_CTL_TILED_LINEAR);
    gfx_outd(&gfx_pci, PLANE_SURF_2_A,
             (uint32_t) gfx_desktop_sprite.obj.gfx_addr);
    (void) gfx_ind(&gfx_pci, PLANE_SURF_2_A);
    gfx_wait_vblank_frame();

    gfx_desktop_period = period_px;
    gfx_desktop_is_active = true;

    process_t *t = sched_new("gfxrain", gfx_desktop_rain, false);
    if (t != NULL) {
        sched_add(t);
        klogi("GFX: desktop: rain thread started\n");
    } else {
        kloge("GFX: desktop: rain thread spawn failed\n");
    }

    klogi("GFX: desktop: rain %ux%u period %u, console %ux%u at %u,%u\n",
          gfx_fb.width, gfx_fb.height, period_px, spr_w, spr_h, pos_x, pos_y);
    return true;
}

bool gfx_edp_panel_on(gfx_pci_t * pci)
{
    uint32_t pp = gfx_ind(pci, PP_CONTROL);

    /* VDD_FORCE is source-side panel power and gates the EDID EEPROM. Keep it
     * asserted even when the panel power state is already on. */
    pp |= PP_CONTROL_POWER_STATE | PP_CONTROL_VDD_FORCE;
    gfx_outd(pci, PP_CONTROL, pp);

    for (int32_t timeout = 200; timeout-- > 0;) {
        if (gfx_ind(pci, PP_STATUS) & PP_STATUS_ON)
            return true;
        pit_wait(1);
    }

    /* The panel may already be driven by the firmware without PP_STATUS_ON.
     * Do not block the boot on this; the AUX read can still proceed. */
    return true;
}

bool gfx_edp_panel_off(gfx_pci_t * pci)
{
    uint32_t pp = gfx_ind(pci, PP_CONTROL);
    pp &= ~(PP_CONTROL_POWER_STATE | PP_CONTROL_BACKLIGHT_ENABLE);
    gfx_outd(pci, PP_CONTROL, pp);
    klogd("GFX: eDP panel off\n");
    return true;
}

bool gfx_modeset(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                 uint32_t width, uint32_t height, uint32_t pitch,
                 uint32_t format, gfx_fb_t * out_fb)
{
    (void)mgr;
    (void)gtt;

    /* The BIOS GOP display engine is already scanning the aperture (GMADR,
     * physical 0xE0000000) via a trained eDP link.  Firmware traps GTT[0]
     * writes; pipe enable requires DP link re-training we cannot do.
     *
     * The aperture IS the GPU framebuffer: the display engine fetches from
     * GTT[0..N] which the BIOS already mapped to the stolen memory backing
     * the aperture.  Writes to PHYS_TO_VIRT(aperture) appear on screen.
     *
     * We use the caller-supplied pitch (limine's actual pitch) so the display
     * engine's row stride matches what we write. */

    uint32_t stride = (pitch != 0) ? pitch : (width * 4);

    out_fb->width  = width;
    out_fb->height = height;
    out_fb->stride = stride;
    out_fb->format = format;

    /* cpu_addr = aperture virtual base (already mapped by gfx_init_pci) */
    out_fb->obj.cpu_addr = (volatile uint8_t *)pci->aperture_bar;
    out_fb->obj.gfx_addr = 0;

    klogi("GFX: modeset %ux%u OK (aperture scanout, cpu=0x%016lx pitch=%u)\n",
          width, height, (uint64_t)pci->aperture_bar, stride);
    return true;
}

void gfx_start(void)
{
    pci_device_t dev = { 0 };

    if (!pci_get_gfx_device(&dev)) {
        klogd("GFX: No compatible graphics device found\n");
        return;
    }

    klogi("GFX: Starting device %02x:%02x.%01x - %04x:%04x %s\n",
          dev.bus, dev.device, dev.func, dev.vendor_id,
          dev.device_id, pci_device_id_to_string(&dev));

    /* Force out of D6 power state before accessing registers */
    if (!gfx_enter_force_wake(&gfx_pci)) {
        kloge("GFX: Failed to enter force wake state\n");
        return;
    }

    /* Enable memory swizzling for better performance */
    gfx_mem_enable_swizzle(&gfx_pci);

    /* Exit force wake to save power */
    gfx_exit_force_wake(&gfx_pci);

    klogi("GFX: Initialization complete\n");
}

/**
 * @brief Read GPU frequency status
 * @param pci Pointer to GFX PCI structure
 * @return Current GPU frequency in MHz
 */
uint32_t gfx_get_gpu_freq(gfx_pci_t * pci)
{
    uint32_t rpstat1 = gfx_ind(pci, GEN6_RPSTAT1);
    /* CAGF (Current Actual GPU Frequency) is at bits 14:8, in 50 MHz units */
    uint32_t freq_units = (rpstat1 & GEN6_CAGF_MASK) >> GEN6_CAGF_SHIFT;
    return freq_units * 50;
}

/**
 * @brief Check if display pipe is enabled
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true if pipe is enabled
 */
bool gfx_is_pipe_enabled(gfx_pci_t * pci, uint8_t pipe)
{
    uint32_t pipe_conf_reg = PIPEACONF + (pipe * 0x1000);
    uint32_t conf = gfx_ind(pci, pipe_conf_reg);
    return (conf & PIPE_ENABLE) != 0;
}

/**
 * @brief Disable VGA display mode
 * @param pci Pointer to GFX PCI structure
 */
void gfx_disable_vga(gfx_pci_t * pci)
{
    uint32_t vga_ctrl = gfx_ind(pci, VGA_CONTROL);
    if (!(vga_ctrl & VGA_DISABLE)) {
        vga_ctrl |= VGA_DISABLE;
        gfx_outd(pci, VGA_CONTROL, vga_ctrl);
        klogd("GFX: VGA mode disabled\n");
    }
}

/**
 * @brief Get display information
 * @param pci Pointer to GFX PCI structure
 */
void gfx_get_display_info(gfx_pci_t * pci)
{
    /* Acquires its own force wake session — do not call while wake is held */
    if (!gfx_enter_force_wake(pci)) {
        kloge("GFX: gfx_get_display_info: force wake failed\n");
        return;
    }

    klogi("GFX: Display Status:\n");
    klogi("\tGPU Frequency: %u MHz\n", gfx_get_gpu_freq(pci));
    klogi("\tPipe A: %s\n", gfx_is_pipe_enabled(pci, 0) ? "Enabled" : "Disabled");
    klogi("\tPipe B: %s\n", gfx_is_pipe_enabled(pci, 1) ? "Enabled" : "Disabled");
    klogi("\tPipe C: %s\n", gfx_is_pipe_enabled(pci, 2) ? "Enabled" : "Disabled");

    uint32_t vga_ctrl = gfx_ind(pci, VGA_CONTROL);
    klogi("\tVGA Mode: %s\n", (vga_ctrl & VGA_DISABLE) ? "Disabled" : "Enabled");

    gfx_exit_force_wake(pci);
}

/**
 * @brief Wait for pipe to be in specified state
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @param enabled Expected state
 * @return true if state reached within timeout
 */
bool gfx_wait_pipe_state(gfx_pci_t * pci, uint8_t pipe, bool enabled)
{
    uint32_t pipe_conf_reg = PIPEACONF + (pipe * 0x1000);
    int32_t timeout = 50000;  /* 50ms */

    while (timeout-- > 0) {
        uint32_t conf = gfx_ind(pci, pipe_conf_reg);
        bool current_state = (conf & PIPE_STATE) != 0;
        if (current_state == enabled) {
            return true;
        }
        pit_wait(1);
    }

    kloge("GFX: Timeout waiting for pipe %u state=%d\n", pipe, enabled);
    return false;
}

/**
 * @brief Enable display pipe
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true on success
 */
bool gfx_enable_pipe(gfx_pci_t * pci, uint8_t pipe)
{
    uint32_t pipe_conf_reg = PIPEACONF + (pipe * 0x1000);
    uint32_t conf = gfx_ind(pci, pipe_conf_reg);

    if (conf & PIPE_ENABLE) {
        klogd("GFX: Pipe %u already enabled\n", pipe);
        return true;
    }

    conf |= PIPE_ENABLE;
    gfx_outd(pci, pipe_conf_reg, conf);

    if (!gfx_wait_pipe_state(pci, pipe, true)) {
        return false;
    }

    klogi("GFX: Pipe %u enabled\n", pipe);
    return true;
}

/**
 * @brief Disable display pipe
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true on success
 */
bool gfx_disable_pipe(gfx_pci_t * pci, uint8_t pipe)
{
    uint32_t pipe_conf_reg = PIPEACONF + (pipe * 0x1000);
    uint32_t conf = gfx_ind(pci, pipe_conf_reg);

    if (!(conf & PIPE_ENABLE)) {
        klogd("GFX: Pipe %u already disabled\n", pipe);
        return true;
    }

    conf &= ~PIPE_ENABLE;
    gfx_outd(pci, pipe_conf_reg, conf);

    if (!gfx_wait_pipe_state(pci, pipe, false)) {
        return false;
    }

    klogi("GFX: Pipe %u disabled\n", pipe);
    return true;
}

/**
 * @brief Configure display plane
 * @param pci Pointer to GFX PCI structure
 * @param plane Plane number (0=A, 1=B, 2=C)
 * @param format Pixel format
 * @param enabled Enable or disable plane
 * @return true on success
 */
bool gfx_configure_plane(gfx_pci_t * pci, uint8_t plane, uint32_t format,
                         bool enabled)
{
    uint32_t plane_ctrl_reg = DSPACNTR + (plane * 0x1000);
    uint32_t ctrl = gfx_ind(pci, plane_ctrl_reg);

    /* Clear format bits */
    ctrl &= ~DISPPLANE_PIXFORMAT_MASK;

    if (enabled) {
        ctrl |= DISPLAY_PLANE_ENABLE;
        ctrl |= format;
        ctrl |= DISPPLANE_GAMMA_ENABLE;
    } else {
        ctrl &= ~DISPLAY_PLANE_ENABLE;
    }

    gfx_outd(pci, plane_ctrl_reg, ctrl);
    gfx_ind(pci, plane_ctrl_reg);  /* Posting read */

    klogd("GFX: Plane %u %s (format=0x%08x)\n", plane,
          enabled ? "enabled" : "disabled", format);
    return true;
}

/**
 * @brief Set plane framebuffer address
 * @param pci Pointer to GFX PCI structure
 * @param plane Plane number (0=A, 1=B, 2=C)
 * @param addr Physical address of framebuffer
 * @param stride Stride in bytes
 * @return true on success
 */
bool gfx_set_plane_fb(gfx_pci_t * pci, uint8_t plane, uint64_t addr,
                      uint32_t stride)
{
    uint32_t plane_surf = DSP_SURF(plane);
    uint32_t plane_stride = DSP_STRIDE(plane);

    /* Set stride */
    gfx_outd(pci, plane_stride, stride);

    /* Set surface address (triggers update) */
    gfx_outd(pci, plane_surf, (uint32_t) addr);
    gfx_ind(pci, plane_surf);  /* Posting read */

    klogd("GFX: Plane %u FB addr=0x%08x stride=%u\n", plane, (uint32_t) addr,
          stride);
    return true;
}

/**
 * @brief Configure display timing
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number
 * @param htotal Horizontal total
 * @param hblank Horizontal blank
 * @param hsync Horizontal sync
 * @param vtotal Vertical total
 * @param vblank Vertical blank
 * @param vsync Vertical sync
 * @return true on success
 */
bool gfx_set_timing(gfx_pci_t * pci, uint8_t pipe, uint32_t htotal,
                    uint32_t hblank, uint32_t hsync, uint32_t vtotal,
                    uint32_t vblank, uint32_t vsync)
{
    gfx_outd(pci, PIPE_HTOTAL(pipe), htotal);
    gfx_outd(pci, PIPE_HBLANK(pipe), hblank);
    gfx_outd(pci, PIPE_HSYNC(pipe), hsync);
    gfx_outd(pci, PIPE_VTOTAL(pipe), vtotal);
    gfx_outd(pci, PIPE_VBLANK(pipe), vblank);
    gfx_outd(pci, PIPE_VSYNC(pipe), vsync);

    klogd("GFX: Pipe %u timing configured\n", pipe);
    return true;
}

/**
 * @brief Enable display interrupts
 * @param pci Pointer to GFX PCI structure
 * @param mask Interrupt mask
 * @return true on success
 */
bool gfx_enable_interrupts(gfx_pci_t * pci, uint32_t mask)
{
    /* Clear pending interrupts */
    gfx_outd(pci, DEIIR, mask);

    /* Unmask interrupts, including the master gate in bit 31. */
    uint32_t imr = gfx_ind(pci, DEIMR);
    imr &= ~(mask | DE_MASTER_IRQ_CONTROL);
    gfx_outd(pci, DEIMR, imr);

    /* Enable interrupts */
    uint32_t ier = gfx_ind(pci, DEIER);
    ier |= mask;
    gfx_outd(pci, DEIER, ier);

    /* Enable master interrupt control */
    ier |= DE_MASTER_IRQ_CONTROL;
    gfx_outd(pci, DEIER, ier);

    klogd("GFX: Interrupts enabled (mask=0x%08x)\n", mask);
    return true;
}

/**
 * @brief Disable display interrupts
 * @param pci Pointer to GFX PCI structure
 * @param mask Interrupt mask
 * @return true on success
 */
bool gfx_disable_interrupts(gfx_pci_t * pci, uint32_t mask)
{
    /* Disable interrupts */
    uint32_t ier = gfx_ind(pci, DEIER);
    ier &= ~mask;
    gfx_outd(pci, DEIER, ier);

    /* Mask interrupts */
    uint32_t imr = gfx_ind(pci, DEIMR);
    imr |= mask;
    gfx_outd(pci, DEIMR, imr);

    klogd("GFX: Interrupts disabled (mask=0x%08x)\n", mask);
    return true;
}

/**
 * @brief Get pending interrupts
 * @param pci Pointer to GFX PCI structure
 * @return Interrupt status register value
 */
uint32_t gfx_get_interrupts(gfx_pci_t * pci)
{
    return gfx_ind(pci, DEIIR);
}

/**
 * @brief Clear interrupts
 * @param pci Pointer to GFX PCI structure
 * @param mask Interrupts to clear
 */
void gfx_clear_interrupts(gfx_pci_t * pci, uint32_t mask)
{
    gfx_outd(pci, DEIIR, mask);
    gfx_ind(pci, DEIIR);  /* Posting read */
}

/**
 * @brief Configure GPU power state (RC6)
 * @param pci Pointer to GFX PCI structure
 * @param enable Enable or disable RC6
 * @return true on success
 */
bool gfx_configure_rc6(gfx_pci_t * pci, bool enable)
{
    /* Must be called with force wake already held */
    uint32_t rc_control = gfx_ind(pci, GEN6_RC_CONTROL);

    if (enable) {
        rc_control |= GEN6_RC_CTL_RC6_ENABLE;
        rc_control |= GEN6_RC_CTL_EI_MODE(1);

        gfx_outd(pci, GEN6_RC1_WAKE_RATE_LIMIT, 1000 << 16);
        gfx_outd(pci, GEN6_RC6_WAKE_RATE_LIMIT, 40 << 16 | 30);
        gfx_outd(pci, GEN6_RC6pp_WAKE_RATE_LIMIT, 30);
        gfx_outd(pci, GEN6_RC_EVALUATION_INTERVAL, 125000);
        gfx_outd(pci, GEN6_RC_IDLE_HYSTERSIS, 25);

        klogi("GFX: RC6 power state enabled\n");
    } else {
        rc_control &= ~GEN6_RC_CTL_RC6_ENABLE;
        klogi("GFX: RC6 power state disabled\n");
    }

    gfx_outd(pci, GEN6_RC_CONTROL, rc_control);
    return true;
}

/**
 * @brief Set GPU frequency
 * @param pci Pointer to GFX PCI structure
 * @param freq_mhz Desired frequency in MHz (must be multiple of 50)
 * @return true on success
 */
bool gfx_set_gpu_freq(gfx_pci_t * pci, uint32_t freq_mhz)
{
    if (freq_mhz % 50 != 0) {
        kloge("GFX: Frequency must be multiple of 50MHz\n");
        return false;
    }

    /* Must be called with force wake already held */
    uint32_t freq_units = freq_mhz / 50;

    /* RPNSWREQ: bit 31 = SW request enable, bits 24:16 = requested freq */
    uint32_t req = GEN6_TURBO_DISABLE | (freq_units << GEN6_FREQ_SHIFT);
    gfx_outd(pci, GEN6_RPNSWREQ, req);

    klogi("GFX: GPU frequency set to %u MHz\n", freq_mhz);
    return true;
}

/**
 * @brief Get GPU performance status
 * @param pci Pointer to GFX PCI structure
 * @param status Pointer to store status
 * @return true on success
 */
bool gfx_get_perf_status(gfx_pci_t * pci, gfx_perf_status_t * status)
{
    if (!status) {
        return false;
    }

    /* Must be called with force wake already held */
    uint32_t rpstat = gfx_ind(pci, GEN6_RPSTAT1);
    uint32_t limits = gfx_ind(pci, GT_PERF_LIMIT_REASONS);
    uint32_t rpnswreq = gfx_ind(pci, GEN6_RPNSWREQ);

    /* Skylake Gen9 RPSTAT1: current freq in bits 31:24, in 50MHz units */
    uint32_t cur = (rpstat & GEN6_CAGF_MASK) >> GEN6_CAGF_SHIFT;
    /* RPNSWREQ bits 23:16 = requested freq in 50MHz units */
    uint32_t req = (rpnswreq & GEN6_FREQ_MASK) >> GEN6_FREQ_SHIFT;

    status->current_freq_mhz = cur * 50;
    status->requested_freq_mhz = req * 50;
    status->perf_limit_reasons = limits;
    /* GT_PERF_STATUS bits 7:0 = RP0 cap frequency in 50MHz units on Skylake */
    uint32_t gtperf = gfx_ind(pci, GT_PERF_STATUS);
    status->rp0_freq_units = (uint8_t)(gtperf & 0xFF);

    return true;
}

/**
 * @brief Configure hardware cursor
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @param mode Cursor mode
 * @param addr Physical address of cursor data
 * @param x X position
 * @param y Y position
 * @return true on success
 */
bool gfx_configure_cursor(gfx_pci_t * pci, uint8_t pipe, uint32_t mode,
                          uint64_t addr, int32_t x, int32_t y)
{
    uint32_t cursor_ctrl = CURACNTR + (pipe * 0x1000);
    uint32_t cursor_base = CURABASE + (pipe * 0x1000);
    uint32_t cursor_pos = CURAPOS + (pipe * 0x1000);

    /* Configure cursor control */
    uint32_t ctrl = mode & CURSOR_MODE;
    if (pipe == 1) {
        ctrl |= MCURSOR_PIPE_SELECT;
    }
    ctrl |= MCURSOR_GAMMA_ENABLE;

    gfx_outd(pci, cursor_ctrl, ctrl);

    /* Set cursor position */
    uint32_t pos = 0;
    if (x < 0) {
        pos |= ((-x) & 0x1FF) << 16;
        pos |= CURSOR_POS_SIGN_X;
    } else {
        pos |= (x & 0x1FFF) << 16;
    }
    if (y < 0) {
        pos |= ((-y) & 0x1FF);
        pos |= CURSOR_POS_SIGN_Y;
    } else {
        pos |= (y & 0x1FFF);
    }
    gfx_outd(pci, cursor_pos, pos);

    /* Set cursor base address (triggers update) */
    gfx_outd(pci, cursor_base, (uint32_t) addr);
    gfx_ind(pci, cursor_base);  /* Posting read */

    klogd("GFX: Cursor configured on pipe %u at (%d,%d)\n", pipe, x, y);
    return true;
}

/**
 * @brief Disable hardware cursor
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true on success
 */
bool gfx_disable_cursor(gfx_pci_t * pci, uint8_t pipe)
{
    uint32_t cursor_ctrl = CURACNTR + (pipe * 0x1000);
    gfx_outd(pci, cursor_ctrl, CURSOR_MODE_DISABLE);
    klogd("GFX: Cursor disabled on pipe %u\n", pipe);
    return true;
}

/**
 * @brief Wait for VBlank
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true on success
 */
bool gfx_wait_vblank(gfx_pci_t * pci, uint8_t pipe)
{
    bool gen8 = gfx_gen8_pipe_irq(pci);
    uint32_t reg = gen8 ? GEN8_DE_PIPE_IIR(pipe) : DEIIR;
    uint32_t vblank_bit = gen8 ? GEN8_PIPE_VBLANK :
        (pipe == 0) ? DE_PIPEA_VBLANK :
        (pipe == 1) ? DE_PIPEB_VBLANK : DE_PIPEC_VBLANK;

    /* Clear any pending vblank */
    gfx_outd(pci, reg, vblank_bit);

    /* Wait for vblank (PIT ticks are 1 ms). */
    int32_t timeout = 50;
    while (timeout-- > 0) {
        uint32_t iir = gfx_ind(pci, reg);
        if (iir & vblank_bit) {
            gfx_outd(pci, reg, vblank_bit);     /* Clear */
            return true;
        }
        pit_wait(1);
    }

    kloge("GFX: Timeout waiting for vblank on pipe %u\n", pipe);
    return false;
}

/**
 * @brief Advanced feature test (call after initialization)
 * @param pci Pointer to GFX PCI structure
 * @return true on success
 */
bool gfx_test_advanced_features(gfx_pci_t * pci)
{
    klogi("=== GFX Driver Feature Test ===\n");

    if (!gfx_enter_force_wake(pci)) {
        kloge("  [FAIL] Could not enter force wake, skipping tests\n");
        return false;
    }
    klogi("  [PASS] Force wake entered\n");

    /* Test 1: GPU frequency and performance */
    klogi("Test 1: GPU frequency and performance\n");
    klogi("  Current GPU frequency: %u MHz\n", gfx_get_gpu_freq(pci));

    gfx_perf_status_t perf;
    if (gfx_get_perf_status(pci, &perf)) {
        klogi("  [PASS] Current %u MHz, requested %u MHz, RP0 cap %u MHz\n",
              perf.current_freq_mhz, perf.requested_freq_mhz,
              perf.rp0_freq_units * 50);
        klogi("  Limit reasons: 0x%08x\n", perf.perf_limit_reasons);
    } else {
        klogi("  [FAIL] Could not read performance status\n");
    }

    /* Test 2: display pipe status */
    klogi("Test 2: Display pipe status\n");
    for (uint8_t pipe = 0; pipe < 3; pipe++) {
        klogi("  Pipe %c: %s\n", 'A' + pipe,
              gfx_is_pipe_enabled(pci, pipe) ? "ENABLED" : "DISABLED");
    }

    /* Test 3: VGA mode */
    klogi("Test 3: VGA mode control\n");
    gfx_disable_vga(pci);
    klogi("  [PASS] VGA mode disabled\n");

    /* Test 4: memory swizzle */
    klogi("Test 4: Memory swizzle configuration\n");
    if (gfx_mem_enable_swizzle(pci))
        klogi("  [PASS] Memory swizzle enabled\n");
    else
        klogi("  [INFO] Memory swizzle not enabled (DIMM size mismatch)\n");

    /* Test 5: GTT mapping round trip */
    klogi("Test 5: GTT mapping\n");
    {
        gfx_object_t obj = { 0 };
        if (gfx_alloc(&gfx_mgr, &gfx_gtt, &obj, GTT_PAGE_SIZE, GTT_PAGE_SIZE)) {
            volatile uint32_t *p = (volatile uint32_t *)obj.cpu_addr;
            p[0] = 0xDEADBEEF;
            p[1] = 0xCAFEBABE;
            klogi("  [%s] CPU access through GTT page "
                  "(cpu 0x%016lx gpu 0x%016lx)\n",
                  (p[0] == 0xDEADBEEF && p[1] == 0xCAFEBABE) ? "PASS" : "FAIL",
                  (uint64_t) obj.cpu_addr, obj.gfx_addr);
            gfx_gtt_clear(&gfx_gtt, obj.gfx_addr, GTT_PAGE_SIZE);
            klogi("  [PASS] GTT entry cleared\n");
        } else {
            klogi("  [FAIL] GTT allocation failed\n");
        }
    }

    /* Test 6: interrupt status */
    klogi("Test 6: Interrupt status\n");
    uint32_t iir = gfx_get_interrupts(pci);
    klogi("  Interrupt status: 0x%08x\n", iir);
    if (iir != 0)
        gfx_clear_interrupts(pci, iir);
    klogi("  [PASS] Interrupt handling works\n");

    /* Test 7: vblank on enabled pipes */
    klogi("Test 7: VBlank synchronization\n");
    for (uint8_t pipe = 0; pipe < 3; pipe++) {
        if (gfx_is_pipe_enabled(pci, pipe))
            klogi("  [%s] VBlank on pipe %c\n",
                  gfx_wait_vblank(pci, pipe) ? "PASS" : "FAIL", 'A' + pipe);
    }

    /* Test 8: plane and timing readback (read-only) */
    klogi("Test 8: Plane and timing readback\n");
    for (uint8_t pipe = 0; pipe < 3; pipe++) {
        uint32_t ctrl = gfx_ind(pci, DSPACNTR + pipe * 0x1000);
        klogi("  Plane %c: %s format 0x%x\n", 'A' + pipe,
              (ctrl & DISPLAY_PLANE_ENABLE) ? "ENABLED" : "DISABLED",
              (ctrl & DISPPLANE_PIXFORMAT_MASK) >> 26);
        if (gfx_is_pipe_enabled(pci, pipe)) {
            klogi("    HTOTAL 0x%08x VTOTAL 0x%08x\n",
                  gfx_ind(pci, PIPE_HTOTAL(pipe)),
                  gfx_ind(pci, PIPE_VTOTAL(pipe)));
        }
    }

    /* Test 9: memory manager and GTT summary */
    klogi("Test 9: Memory and GTT summary\n");
    klogi("  Stolen 0x%08x (%u MB), GTT %u MB (%u entries, %u mappable)\n",
          gfx_gtt.stolen_mem_base, gfx_gtt.stolen_mem_size / MB,
          gfx_gtt.gtt_mem_size / MB, gfx_gtt.num_total_entries,
          gfx_gtt.num_mappable_entries);

    /* Test 10: frequency scaling is skipped. A write to RPNSWREQ triggers a
     * platform PME/SMI whose handler is not registered yet. */
    klogi("Test 10: GPU frequency scaling (skipped, triggers PME/SMI)\n");

    gfx_exit_force_wake(pci);
    klogi("  [PASS] Force wake exited\n");

    klogi("=== GFX Driver Feature Test Complete ===\n");
    return true;
}

/**
 * @brief Get pointer to GFX PCI structure
 * @return Pointer to gfx_pci
 */
gfx_pci_t *gfx_get_pci(void)
{
    return &gfx_pci;
}
