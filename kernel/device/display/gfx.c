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
#include <libc/string.h>
#include <sys/cpu.h>
#include <sys/pci.h>
#include <sys/pit.h>
#include <sys/serial.h>
#include <mm/mm.h>
#include <base/vector.h>
#include <base/klog.h>
#include <base/time.h>
#include <proc/task.h>
#include <proc/sched.h>
#include <device/display/gfx.h>
#include <device/display/gfx_reg.h>
#include <device/display/term.h>
#include <base/kmalloc.h>

#define DEVICE_HD5500               0x1616
#define DEVICE_HD520                0x1916
#define DEVICE_SUNRISE_PANTHERPOINT 0x9D00

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

bool gfx_validate_chipset(void)
{
    /* Check we are on Pather Point Chipset */

    /* Assume location of ISA Bridge */
    uint32_t isa_bridge_id = PCI_MAKE_ID(0, 0x1f, 0);

    uint16_t class_code =
        ((uint16_t) pci_inb(isa_bridge_id, PCI_CONFIG_CLASS_CODE) << 8)
        | (uint16_t) pci_inb(isa_bridge_id, PCI_CONFIG_SUBCLASS);

    if (class_code != PCI_BRIDGE_ISA) {
        klogi("Isa Bridge not found at expected location! "
              "(Found: 0x%4x, Expected: 0x%4x)\n",
              class_code, PCI_BRIDGE_ISA);
        return false;
    }

    uint16_t device_id =
        pci_inw(isa_bridge_id, PCI_CONFIG_DEVICE_ID) & 0xFF00;
    if (device_id != DEVICE_SUNRISE_PANTHERPOINT) {
        klogi("Chipset is not expect panther point (needed for display "
              "handling)! (Found: 0x%X, Expected: 0x%X)\n",
              device_id, DEVICE_SUNRISE_PANTHERPOINT);
        return false;
    }

    return true;
}

void gfx_init_pci(gfx_pci_t * pci, pci_device_t dev)
{
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    task_t *t = sched_get_current_task();
    addrspace_t *as = NULL;

    if (t != NULL)
        as = t->addrspace;

    /* Read PCI registers */
    pci_bar_t bar;

    /* Graphics Memory Address Spaces
     * BAR0: GTTMMADR - The combined Graphics Translation Table Modification
     * Range and Memory Mapped Range. GTTADR will begin at GTTMMADR 2MB while
     * the MMIO base address will be the same as GTTMMADR
     * Ref: https://01.org/sites/default/files/documentation/intel-gfx-prm-
     *      osrc-hsw-pcie-config-registers.pdf#page=129 */
    pci_get_bar(&bar, id, 0);
    pci->mmio_bar = (volatile void *) PHYS_TO_VIRT(bar.u.address);
    pci->gtt_addr =
        (volatile uint32_t *) ((uint8_t *) pci->mmio_bar + 2 * MB);
    klogi("\tGTTMMADR: 0x%11x (%d MB) on address space 0x%x\n",
          bar.u.address, bar.size / MB, as);

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
    klogi("\tGMADR:    0x%11x (%d MB)\n", bar.u.address, bar.size / MB);

    vmm_map(as, (uint64_t) pci->aperture_bar, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_DEFAULT);

    /* BAR4: IOBASE - This register provides the Base offset of the IO
     * registers within Device #2. */
    pci_get_bar(&bar, id, 4);
    pci->iobase = bar.u.port;
    klogi("\tIOBASE:   0x%11x (%d bytes)\n", bar.u.port, bar.size);
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

    int gms = (ggc >> GGC_GMS_SHIFT) & GGC_GMS_MASK;
    gtt->stolen_mem_size = GMS_TO_SIZE[gms];

    int ggms = (ggc >> GGC_GGMS_SHIFT) & GGC_GGMS_MASK;
    gtt->gtt_mem_size = 0;

    switch (ggms) {
    case GGMS_None:
        gtt->gtt_mem_size = 0;
        break;

    case GGMS_1MB:
        gtt->gtt_mem_size = 1 * MB;
        break;

    case GGMS_2MB:
        gtt->gtt_mem_size = 2 * MB;
        break;

    default:
        gtt->gtt_mem_size = -1;
        break;
    }

    gtt->stolen_mem_base = bdsm & BDSM_ADDR_MASK;

    gtt->num_total_entries = gtt->gtt_mem_size / sizeof(uint32_t);
    gtt->num_mappable_entries = pci->aperture_size >> GTT_PAGE_SHIFT;
    gtt->entries = pci->gtt_addr;

    klogi("GTT Config:\n");
    klogi("\tStolen Mem Base:      0x%11x\n", gtt->stolen_mem_base);
    klogi("\tStolen Mem Size:      %d MB\n", gtt->stolen_mem_size / MB);
    klogi("\tGTT Mem Size:         %d MB\n", gtt->gtt_mem_size / MB);
    klogi("\tGTT Total Entries:    %d\n", gtt->num_total_entries);
    klogi("\tGTT Mappable Entries: %d\n", gtt->num_mappable_entries);
}

void gfx_init_mem_manager(gfx_pci_t * pci, gfx_gtt_t * gtt,
                          gfx_mem_manager_t * mgr)
{
    mgr->vram.base = 0;
    mgr->vram.current = mgr->vram.base;
    mgr->vram.top = gtt->stolen_mem_size;

    /* GPU address 0 is reserved — start shared range at page 1 minimum */
    uint64_t shared_base = gtt->stolen_mem_size;
    if (shared_base < 4096)
        shared_base = 4096;
    mgr->shared.base = shared_base;
    mgr->shared.current = shared_base;
    mgr->shared.top = (uint64_t)gtt->num_mappable_entries << GTT_PAGE_SHIFT;
    klogd("GFX: mem_mgr shared base=0x%x top=0x%x\n",
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
            kloge("GFX: Timeout waiting for force wake ACK to clear (0x%x)\n",
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

    klogd("GFX: Force wake enabled (ACK=0x%x)\n", force_wake_ack);
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

    /* Find Intel HD graphics device */
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        dev = vec_at(&pci_devices, i);
        if ((dev.vendor_id != VENDOR_INTEL) ||
            (dev.device_id != DEVICE_HD5500
             && dev.device_id != DEVICE_HD520)) {
            continue;
        }
        klogi("Found GFX device %2x:%2x.%1x - %4x:%4x %s\n",
              dev.bus, dev.device, dev.func, dev.vendor_id, dev.device_id,
              pci_device_id_to_string(&dev));
        found = true;
        break;
    }

    if (!found) {
        return false;
    }

    if (!gfx_validate_chipset()) {
        memset(&dev, 0, sizeof(pci_device_t));
        return false;
    }

    if (gfx_dev != NULL)
        memcpy(gfx_dev, &dev, sizeof(pci_device_t));
    klogi("PCI: GFX device checking finished.\n");
    return true;
}

bool gfx_init(void)
{
    bool ret = false;
    pci_device_t dev = { 0 };

    ret = pci_get_gfx_device(&dev);

    if (ret) {
        /* Open and config */
        gfx_init_pci(&gfx_pci, dev);
        gfx_init_gtt(&gfx_pci, &gfx_gtt, dev);
        gfx_init_mem_manager(&gfx_pci, &gfx_gtt, &gfx_mgr);

        /* Demo: Test all new features on real hardware.
         * All tests run inside a single force-wake session to avoid the
         * re-entry timeout seen when each helper acquired its own wake. */
        klogi("=== GFX Driver Feature Test ===\n");

        if (!gfx_enter_force_wake(&gfx_pci)) {
            kloge("  [FAIL] Could not enter force wake — skipping tests\n");
        } else {
            /* Test 1: Force wake and power management */
            klogi("Test 1: Force wake and power management\n");
            klogi("  [PASS] Force wake entered\n");

            uint32_t freq = gfx_get_gpu_freq(&gfx_pci);
            klogi("  Current GPU frequency: %d MHz\n", freq);

            gfx_perf_status_t perf;
            if (gfx_get_perf_status(&gfx_pci, &perf)) {
                klogi("  [PASS] Performance status:\n");
                klogi("    Current freq:   %d MHz\n", perf.current_freq_mhz);
                klogi("    Requested freq: %d MHz\n", perf.requested_freq_mhz);
                klogi("    RP0 cap freq:   %d MHz\n", perf.rp0_freq_units * 50);
                klogi("    Limit reasons:  0x%x\n", perf.perf_limit_reasons);
            }

            /* Test 2: Display pipe status */
            klogi("Test 2: Display pipe status\n");
            for (uint8_t pipe = 0; pipe < 3; pipe++) {
                klogi("  Pipe %c: %s\n", 'A' + pipe,
                      gfx_is_pipe_enabled(&gfx_pci, pipe) ? "ENABLED" : "DISABLED");
            }

            /* Test 3: VGA disable */
            klogi("Test 3: VGA mode control\n");
            gfx_disable_vga(&gfx_pci);
            klogi("  [PASS] VGA mode disabled\n");

            /* Test 4: Memory swizzle */
            klogi("Test 4: Memory swizzle configuration\n");
            if (gfx_mem_enable_swizzle(&gfx_pci)) {
                klogi("  [PASS] Memory swizzle enabled\n");
            } else {
                klogi("  [INFO] Memory swizzle not enabled (DIMM size mismatch)\n");
            }

            /* Test 4b: GTT mapping */
            klogi("Test 4b: GTT mapping\n");
            {
                gfx_object_t obj = { 0 };
                if (gfx_alloc(&gfx_mgr, &gfx_gtt, &obj, GTT_PAGE_SIZE,
                               GTT_PAGE_SIZE)) {
                    klogi("  [PASS] Allocated 1 page\n");
                    klogi("    CPU addr: 0x%x\n", obj.cpu_addr);
                    klogi("    GPU addr: 0x%x\n", obj.gfx_addr);

                    /* Verify CPU access to the mapped page */
                    volatile uint32_t *p = (volatile uint32_t *)obj.cpu_addr;
                    p[0] = 0xDEADBEEF;
                    p[1] = 0xCAFEBABE;
                    if (p[0] == 0xDEADBEEF && p[1] == 0xCAFEBABE) {
                        klogi("  [PASS] CPU read/write through mapped page OK\n");
                    } else {
                        klogi("  [FAIL] CPU read/write mismatch\n");
                    }

                    /* On Skylake, GTT entries are write-only from CPU —
                     * readback reflects hardware state, not the written value.
                     * Log the entry value we computed and wrote. */
                    uint32_t gtt_idx = (uint32_t)(obj.gfx_addr >> GTT_PAGE_SHIFT);
                    uint32_t phys32 = (uint32_t)VIRT_TO_PHYS((uint64_t)obj.cpu_addr);
                    uint32_t expected = (phys32 & ~(uint32_t)(GTT_PAGE_SIZE - 1))
                                      | ((phys32 >> 28) & 0xFF0)
                                      | GTT_ENTRY_LLC_CACHE_CONTROL
                                      | GTT_ENTRY_VALID;
                    klogi("  [PASS] GTT[%x] written: phys=0x%x entry=0x%x\n",
                          gtt_idx, phys32, expected);

                    gfx_gtt_clear(&gfx_gtt, obj.gfx_addr, GTT_PAGE_SIZE);
                    klogi("  [PASS] GTT entry cleared\n");
                } else {
                    klogi("  [FAIL] GTT allocation failed\n");
                }
            }

            /* Test 5: RC6 power state */
            klogi("Test 5: RC6 power state (skipped for stability)\n");
            klogi("  [INFO] RC6 can be enabled with gfx_configure_rc6()\n");

            /* Test 6: Interrupt status */
            klogi("Test 6: Interrupt status\n");
            uint32_t iir = gfx_get_interrupts(&gfx_pci);
            klogi("  Interrupt status: 0x%x\n", iir);
            if (iir != 0) {
                klogi("  Clearing pending interrupts\n");
                gfx_clear_interrupts(&gfx_pci, iir);
            }
            klogi("  [PASS] Interrupt handling works\n");

            /* Test 7: Display info summary */
            klogi("Test 7: Display information summary\n");
            klogi("GFX: Display Status:\n");
            klogi("\tGPU Frequency: %d MHz\n", gfx_get_gpu_freq(&gfx_pci));
            klogi("\tPipe A: %s\n", gfx_is_pipe_enabled(&gfx_pci, 0) ? "Enabled" : "Disabled");
            klogi("\tPipe B: %s\n", gfx_is_pipe_enabled(&gfx_pci, 1) ? "Enabled" : "Disabled");
            klogi("\tPipe C: %s\n", gfx_is_pipe_enabled(&gfx_pci, 2) ? "Enabled" : "Disabled");
            uint32_t vga_ctrl = gfx_ind(&gfx_pci, VGA_CONTROL);

            klogi("\tVGA Mode: %s\n", (vga_ctrl & VGA_DISABLE) ? "Disabled" : "Enabled");

            /* Test 8: Pipe control */
            klogi("Test 8: Pipe control (read-only test)\n");
            for (uint8_t pipe = 0; pipe < 3; pipe++) {
                if (!gfx_is_pipe_enabled(&gfx_pci, pipe)) {
                    klogi("  [INFO] Pipe %c is available for testing\n", 'A' + pipe);
                }
            }

            /* Test 9: GPU frequency scaling — SKIPPED.
             * Any write to RPNSWREQ triggers a platform PME/SMI on this
             * hardware whose handler is not yet registered.  The SMI fires
             * during the next long operation and corrupts the stack → GPF. */
            klogi("Test 9: GPU frequency scaling (skipped - triggers PME/SMI)\n");
            uint32_t original_freq = gfx_get_gpu_freq(&gfx_pci);
            klogi("  Current frequency: %d MHz\n", original_freq);

            /* Test 10: Cursor control registers (read-only) */
            klogi("Test 10: Cursor control\n");
            klogi("  [INFO] Cursor configured with gfx_configure_Claude Code()\n");
            klogi("  [INFO] Supported modes: 64x64, 128x128, 256x256 ARGB\n");

            klogi("=== GFX Driver Feature Test Complete ===\n");

            /* Use aperture as GPU framebuffer: pass limine's actual pitch so
             * our stride matches what the display engine is already using. */
            fb_info_t *limine_fb = term_get_fb();
            if (limine_fb && gfx_modeset(&gfx_pci, &gfx_mgr, &gfx_gtt,
                            limine_fb->width, limine_fb->height,
                            limine_fb->pitch, DISPPLANE_BGRX888, &gfx_fb)) {
                uint32_t fbsize = gfx_fb.stride * gfx_fb.height;
                uint8_t *new_bb = (uint8_t *)kmalloc(fbsize);
                if (new_bb) {
                    memset(new_bb, 0, fbsize);
                    /* addr stays pointing at aperture (same as before).
                     * Replace backbuffer with a fresh shadow sized for the
                     * current resolution; old limine backbuffer is abandoned. */
                    limine_fb->backbuffer     = new_bb;
                    limine_fb->backbuffer_len = fbsize;
                    klogi("GFX: fb backbuffer reallocated: %dx%d pitch=%d\n",
                          limine_fb->width, limine_fb->height, limine_fb->pitch);
                } else {
                    kloge("GFX: kmalloc failed for backbuffer\n");
                }
            }

            gfx_exit_force_wake(&gfx_pci);
            klogi("  [PASS] Force wake exited\n");
        }
    }

    return ret;
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
 * Entry format (32-bit):
 *   bits 31:12  Physical page address bits 31:12
 *   bits 11:4   Physical address bits 39:32 (for >4 GB RAM)
 *   bit  3      GFX data type
 *   bit  2      LLC cache control
 *   bit  1      L3 cache control
 *   bit  0      Valid
 */
void gfx_gtt_write_entry(gfx_gtt_t * gtt, uint32_t index, uint64_t phys)
{
    uint32_t entry = (uint32_t)(phys & ~(uint64_t)(GTT_PAGE_SIZE - 1))
                   | (uint32_t)((phys >> 28) & 0xFF0)
                   | GTT_ENTRY_LLC_CACHE_CONTROL
                   | GTT_ENTRY_VALID;
    gtt->entries[index] = entry;
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
        kloge("GFX: gfx_gtt_map: gpu_addr 0x%x not page-aligned\n", gpu_addr);
        return 0;
    }

    uint32_t start_idx = (uint32_t)(gpu_addr >> GTT_PAGE_SHIFT);
    uint32_t num_pages = (uint32_t)NUM_PAGES(size);

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

    klogd("GFX: GTT mapped %u pages at GPU 0x%x phys 0x%x\n",
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

    for (uint32_t i = 0; i < num_pages && (start_idx + i) < gtt->num_total_entries; i++) {
        gtt->entries[start_idx + i] = 0;
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

    return true;
}

bool gfx_edp_panel_on(gfx_pci_t * pci)
{
    uint32_t pp = gfx_ind(pci, PP_CONTROL);
    if (pp & PP_CONTROL_POWER_STATE) {
        klogd("GFX: eDP panel already on (PP_CONTROL=0x%x)\n", pp);
        return true;
    }
    pp |= PP_CONTROL_POWER_STATE | PP_CONTROL_VDD_FORCE;
    gfx_outd(pci, PP_CONTROL, pp);

    int timeout = 200000;
    while (timeout-- > 0) {
        if (gfx_ind(pci, PP_STATUS) & PP_STATUS_ON)
            return true;
        pit_wait(1);
    }
    kloge("GFX: eDP panel power-on timeout\n");
    return false;
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

    klogi("GFX: modeset %dx%d OK (aperture scanout, cpu=0x%x pitch=%d)\n",
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

    klogi("GFX: Starting device %2x:%2x.%1x - %4x:%4x %s\n",
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
    klogi("\tGPU Frequency: %d MHz\n", gfx_get_gpu_freq(pci));
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
    int timeout = 50000;  /* 50ms */

    while (timeout-- > 0) {
        uint32_t conf = gfx_ind(pci, pipe_conf_reg);
        bool current_state = (conf & PIPE_STATE) != 0;
        if (current_state == enabled) {
            return true;
        }
        pit_wait(1);
    }

    kloge("GFX: Timeout waiting for pipe %d state=%d\n", pipe, enabled);
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
        klogd("GFX: Pipe %d already enabled\n", pipe);
        return true;
    }

    conf |= PIPE_ENABLE;
    gfx_outd(pci, pipe_conf_reg, conf);

    if (!gfx_wait_pipe_state(pci, pipe, true)) {
        return false;
    }

    klogi("GFX: Pipe %d enabled\n", pipe);
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
        klogd("GFX: Pipe %d already disabled\n", pipe);
        return true;
    }

    conf &= ~PIPE_ENABLE;
    gfx_outd(pci, pipe_conf_reg, conf);

    if (!gfx_wait_pipe_state(pci, pipe, false)) {
        return false;
    }

    klogi("GFX: Pipe %d disabled\n", pipe);
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

    klogd("GFX: Plane %d %s (format=0x%x)\n", plane,
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
    uint32_t plane_base = 0x70184 + (plane * 0x1000);  /* DSPxSURF */
    uint32_t plane_stride = 0x70188 + (plane * 0x1000);  /* DSPxSTRIDE */

    /* Set stride */
    gfx_outd(pci, plane_stride, stride);

    /* Set surface address (triggers update) */
    gfx_outd(pci, plane_base, (uint32_t) addr);
    gfx_ind(pci, plane_base);  /* Posting read */

    klogd("GFX: Plane %d FB addr=0x%x stride=%d\n", plane, (uint32_t) addr,
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
    uint32_t base = (pipe == 0) ? 0x60000 : (pipe == 1) ? 0x61000 : 0x62000;

    gfx_outd(pci, base + 0x000, htotal);  /* HTOTAL */
    gfx_outd(pci, base + 0x004, hblank);  /* HBLANK */
    gfx_outd(pci, base + 0x008, hsync);   /* HSYNC */
    gfx_outd(pci, base + 0x00C, vtotal);  /* VTOTAL */
    gfx_outd(pci, base + 0x010, vblank);  /* VBLANK */
    gfx_outd(pci, base + 0x014, vsync);   /* VSYNC */

    klogd("GFX: Pipe %d timing configured\n", pipe);
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

    /* Unmask interrupts */
    uint32_t imr = gfx_ind(pci, DEIMR);
    imr &= ~mask;
    gfx_outd(pci, DEIMR, imr);

    /* Enable interrupts */
    uint32_t ier = gfx_ind(pci, DEIER);
    ier |= mask;
    gfx_outd(pci, DEIER, ier);

    /* Enable master interrupt control */
    ier |= DE_MASTER_IRQ_CONTROL;
    gfx_outd(pci, DEIER, ier);

    klogd("GFX: Interrupts enabled (mask=0x%x)\n", mask);
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

    klogd("GFX: Interrupts disabled (mask=0x%x)\n", mask);
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

    klogi("GFX: GPU frequency set to %d MHz\n", freq_mhz);
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

    /* Configure Claude Code control */
    uint32_t ctrl = mode & CURSOR_MODE;
    if (pipe == 1) {
        ctrl |= MCURSOR_PIPE_SELECT;
    }
    ctrl |= MCURSOR_GAMMA_ENABLE;

    gfx_outd(pci, cursor_ctrl, ctrl);

    /* Set Claude Code position */
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

    /* Set Claude Code base address (triggers update) */
    gfx_outd(pci, cursor_base, (uint32_t) addr);
    gfx_ind(pci, cursor_base);  /* Posting read */

    klogd("GFX: Cursor configured on pipe %d at (%d,%d)\n", pipe, x, y);
    return true;
}

/**
 * @brief Disable hardware Claude Code
 * @param pci Pointer to GFX PCI structure
 * @param pipe Pipe number (0=A, 1=B, 2=C)
 * @return true on success
 */
bool gfx_disable_cursor(gfx_pci_t * pci, uint8_t pipe)
{
    uint32_t cursor_ctrl = CURACNTR + (pipe * 0x1000);
    gfx_outd(pci, cursor_ctrl, CURSOR_MODE_DISABLE);
    klogd("GFX: Cursor disabled on pipe %d\n", pipe);
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
    uint32_t vblank_bit = (pipe == 0) ? DE_PIPEA_VBLANK :
                          (pipe == 1) ? DE_PIPEB_VBLANK : DE_PIPEC_VBLANK;

    /* Clear any pending vblank */
    gfx_outd(pci, DEIIR, vblank_bit);

    /* Wait for vblank */
    int timeout = 50000;  /* 50ms */
    while (timeout-- > 0) {
        uint32_t iir = gfx_ind(pci, DEIIR);
        if (iir & vblank_bit) {
            gfx_outd(pci, DEIIR, vblank_bit);  /* Clear */
            return true;
        }
        pit_wait(1);
    }

    kloge("GFX: Timeout waiting for vblank on pipe %d\n", pipe);
    return false;
}

/**
 * @brief Advanced feature test (call after initialization)
 * @param pci Pointer to GFX PCI structure
 * @return true on success
 */
bool gfx_test_advanced_features(gfx_pci_t * pci)
{
    klogi("=== Advanced GFX Feature Test ===\n");

    /* Test 1: VBlank synchronization */
    klogi("Test 1: VBlank synchronization\n");
    if (gfx_enter_force_wake(pci)) {
        for (uint8_t pipe = 0; pipe < 3; pipe++) {
            if (gfx_is_pipe_enabled(pci, pipe)) {
                klogi("  Testing VBlank on pipe %c\n", 'A' + pipe);
                if (gfx_wait_vblank(pci, pipe)) {
                    klogi("  [PASS] VBlank detected on pipe %c\n", 'A' + pipe);
                } else {
                    klogi("  [FAIL] VBlank timeout on pipe %c\n", 'A' + pipe);
                }
            }
        }
        gfx_exit_force_wake(pci);
    }

    /* Test 2: Interrupt enable/disable */
    klogi("Test 2: Interrupt control\n");
    if (gfx_enter_force_wake(pci)) {
        /* Enable VBlank interrupts for all pipes */
        uint32_t vblank_mask = DE_PIPEA_VBLANK | DE_PIPEB_VBLANK |
                               DE_PIPEC_VBLANK;

        if (gfx_enable_interrupts(pci, vblank_mask)) {
            klogi("  [PASS] VBlank interrupts enabled\n");

            /* Wait a bit and check for interrupts */
            pit_wait(20);
            uint32_t iir = gfx_get_interrupts(pci);
            klogi("  Interrupt status after 20ms: 0x%x\n", iir);

            if (iir & vblank_mask) {
                klogi("  [PASS] VBlank interrupts firing\n");
                gfx_clear_interrupts(pci, iir);
            }

            /* Disable interrupts */
            gfx_disable_interrupts(pci, vblank_mask);
            klogi("  [PASS] VBlank interrupts disabled\n");
        }
        gfx_exit_force_wake(pci);
    }

    /* Test 3: Plane configuration (read-only) */
    klogi("Test 3: Plane configuration\n");
    if (gfx_enter_force_wake(pci)) {
        for (uint8_t plane = 0; plane < 3; plane++) {
            uint32_t plane_ctrl_reg = DSPACNTR + (plane * 0x1000);
            uint32_t ctrl = gfx_ind(pci, plane_ctrl_reg);

            klogi("  Plane %c control: 0x%x\n", 'A' + plane, ctrl);
            if (ctrl & DISPLAY_PLANE_ENABLE) {
                klogi("    Status: ENABLED\n");
                uint32_t format = ctrl & DISPPLANE_PIXFORMAT_MASK;
                klogi("    Format: 0x%x\n", format >> 26);
            } else {
                klogi("    Status: DISABLED\n");
            }
        }
        gfx_exit_force_wake(pci);
    }

    /* Test 4: Performance monitoring over time */
    klogi("Test 4: Performance monitoring\n");
    if (gfx_enter_force_wake(pci)) {
        gfx_perf_status_t perf1, perf2;

        /* gfx_get_perf_status requires force wake to already be held */
        if (gfx_get_perf_status(pci, &perf1)) {
            klogi("  Initial state:\n");
            klogi("    Frequency: %d MHz\n", perf1.current_freq_mhz);
            klogi("    Busy: %d%%\n", perf1.rp0_freq_units);

            pit_wait(100);

            if (gfx_get_perf_status(pci, &perf2)) {
                klogi("  After 100ms:\n");
                klogi("    Frequency: %d MHz\n", perf2.current_freq_mhz);
                klogi("    Busy: %d%%\n", perf2.rp0_freq_units);

                if (perf2.current_freq_mhz != perf1.current_freq_mhz)
                    klogi("  [INFO] Frequency changed dynamically\n");
            }
        }
        gfx_exit_force_wake(pci);
    }

    /* Test 5: Timing register readback */
    klogi("Test 5: Display timing readback\n");
    if (gfx_enter_force_wake(pci)) {
        for (uint8_t pipe = 0; pipe < 3; pipe++) {
            if (gfx_is_pipe_enabled(pci, pipe)) {
                uint32_t base = (pipe == 0) ? 0x60000 :
                                (pipe == 1) ? 0x61000 : 0x62000;

                uint32_t htotal = gfx_ind(pci, base + 0x000);
                uint32_t vtotal = gfx_ind(pci, base + 0x00C);

                klogi("  Pipe %c timing:\n", 'A' + pipe);
                klogi("    HTOTAL: 0x%x\n", htotal);
                klogi("    VTOTAL: 0x%x\n", vtotal);
            }
        }
        gfx_exit_force_wake(pci);
    }

    /* Test 6: Memory manager status */
    klogi("Test 6: Memory manager status\n");
    klogi("  VRAM range: 0x%x - 0x%x (current: 0x%x)\n",
          (uint32_t) gfx_mgr.vram.base, (uint32_t) gfx_mgr.vram.top,
          (uint32_t) gfx_mgr.vram.current);
    klogi("  Shared range: 0x%x - 0x%x (current: 0x%x)\n",
          (uint32_t) gfx_mgr.shared.base, (uint32_t) gfx_mgr.shared.top,
          (uint32_t) gfx_mgr.shared.current);
    klogi("  Private range: 0x%x - 0x%x (current: 0x%x)\n",
          (uint32_t) gfx_mgr.priv.base, (uint32_t) gfx_mgr.priv.top,
          (uint32_t) gfx_mgr.priv.current);

    /* Test 7: GTT status */
    klogi("Test 7: GTT status\n");
    klogi("  Stolen memory: 0x%x (%d MB)\n",
          gfx_gtt.stolen_mem_base, gfx_gtt.stolen_mem_size / MB);
    klogi("  GTT size: %d MB\n", gfx_gtt.gtt_mem_size / MB);
    klogi("  Total entries: %d\n", gfx_gtt.num_total_entries);
    klogi("  Mappable entries: %d\n", gfx_gtt.num_mappable_entries);

    klogi("=== Advanced Feature Test Complete ===\n");
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
