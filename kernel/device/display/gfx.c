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

#define DEVICE_HD5500               0x1616
#define DEVICE_HD520                0x1916
#define DEVICE_SUNRISE_PANTHERPOINT 0x9D00

static const uint32_t GMS_TO_SIZE[] =
{
      0 * MB,     /* GMS_0MB */
     32 * MB,     /* GMS_32MB_1 */
     64 * MB,     /* GMS_64MB_1 */
     96 * MB,     /* GMS_96MB_1 */
    128 * MB,     /* GMS_128MB_1 */
     32 * MB,     /* GMS_32MB */
     48 * MB,     /* GMS_48MB */
     64 * MB,     /* GMS_64MB */
    128 * MB,     /* GMS_128MB */
    256 * MB,     /* GMS_256MB */
     96 * MB,     /* GMS_96MB */
    160 * MB,     /* GMS_160MB */
    224 * MB,     /* GMS_224MB */
    352 * MB,     /* GMS_352MB */
    448 * MB,     /* GMS_448MB */
    480 * MB,     /* GMS_480MB */
    512 * MB,     /* GMS_512MB */
};

vec_extern(pci_device_t, pci_devices);

static gfx_pci_t gfx_pci = {0};
static gfx_gtt_t gfx_gtt = {0};
static gfx_mem_manager_t gfx_mgr = {0};

bool gfx_validate_chipset(void)
{
    /* Check we are on Pather Point Chipset */

    /* Assume location of ISA Bridge */
    uint32_t isa_bridge_id = PCI_MAKE_ID(0, 0x1f, 0);

    uint16_t class_code = 
        ((uint16_t)pci_inb(isa_bridge_id, PCI_CONFIG_CLASS_CODE) << 8)
        | (uint16_t)pci_inb(isa_bridge_id, PCI_CONFIG_SUBCLASS);

    if (class_code != PCI_BRIDGE_ISA) {
        klogi("Isa Bridge not found at expected location! "
              "(Found: 0x%4x, Expected: 0x%4x)\n",
              class_code, PCI_BRIDGE_ISA);
        return false;
    }

    uint16_t device_id = pci_inw(isa_bridge_id, PCI_CONFIG_DEVICE_ID) & 0xFF00;
    if (device_id != DEVICE_SUNRISE_PANTHERPOINT)
    {
        klogi("Chipset is not expect panther point (needed for display "
              "handling)! (Found: 0x%X, Expected: 0x%X)\n",
              device_id, DEVICE_SUNRISE_PANTHERPOINT);
        return false;
    }

    return true;
}

void gfx_init_pci(gfx_pci_t* pci, pci_device_t dev)
{
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    task_t *t = sched_get_current_task();
    addrspace_t *as = NULL;

    if (t != NULL) as = t->addrspace;

    /* Read PCI registers */
    pci_bar_t bar;

    /* Graphics Memory Address Spaces
     * BAR0: GTTMMADR - The combined Graphics Translation Table Modification
     * Range and Memory Mapped Range. GTTADR will begin at GTTMMADR 2MB while
     * the MMIO base address will be the same as GTTMMADR
     * Ref: https://01.org/sites/default/files/documentation/intel-gfx-prm-
     *      osrc-hsw-pcie-config-registers.pdf#page=129 */
    pci_get_bar(&bar, id, 0);
    pci->mmio_bar = (volatile void*)PHYS_TO_VIRT(bar.u.address);
    pci->gtt_addr = (volatile uint32_t*)((uint8_t*)pci->mmio_bar + 2 * MB);
    klogi("\tGTTMMADR: 0x%11x (%d MB) on address space 0x%x\n",
          bar.u.address, bar.size / MB, as);

    vmm_map(as, (uint64_t)pci->mmio_bar, (uint64_t)bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO);

    /* BAR2: GMADR - Address range allocated via the Device 2 (integrated
     * graphics device) GMADR register. The processor and other peer (DMI)
     * devices utilize this address space to read/write graphics data that
     * resides in Main Memory. This address is internally converted to a
     * GM_Address. */
    pci_get_bar(&bar, id, 2);
    pci->aperture_bar = (volatile void*)PHYS_TO_VIRT(bar.u.address);
    pci->aperture_size = bar.size;
    klogi("\tGMADR:    0x%11x (%d MB)\n", bar.u.address, bar.size / MB);

    vmm_map(as, (uint64_t)pci->aperture_bar, (uint64_t)bar.u.address,
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
void gfx_init_gtt(gfx_pci_t* pci, gfx_gtt_t* gtt, pci_device_t dev)
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

void gfx_init_mem_manager(gfx_pci_t* pci, gfx_gtt_t* gtt, gfx_mem_manager_t *mgr)
{
    mgr->vram.base       = 0;
    mgr->vram.current    = mgr->vram.base;
    mgr->vram.top        = gtt->stolen_mem_size;

    mgr->shared.base     = gtt->stolen_mem_size;
    mgr->shared.current  = mgr->shared.base;
    mgr->shared.top      = gtt->num_mappable_entries << GTT_PAGE_SHIFT;

    mgr->priv.base       = gtt->num_mappable_entries << GTT_PAGE_SHIFT;
    mgr->priv.current    = mgr->priv.base;
    mgr->priv.top        = ((uint64_t)gtt->num_total_entries) << GTT_PAGE_SHIFT;

    /* Clear all fence registers (provide linear access to mem to cpu) */
    for (uint64_t fence_num = 0; fence_num < FENCE_COUNT; fence_num++) {
        gfx_outl(pci, FENCE_BASE + sizeof(uint64_t) * fence_num, 0);
    }

    mgr->gfx_mem_base = pci->aperture_bar;
    mgr->gfx_mem_next = mgr->gfx_mem_base + 4 * GTT_PAGE_SIZE;
}

void gfx_enter_force_wake(gfx_pci_t* pci)
{
    kprintf("Trying to entering force wake...\n");

    int64_t trys = 0;
    uint32_t force_wake_ack = 0;
    do {
        ++trys;
        force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xFFFF;
        kprintf("Waiting for Force Ack to Clear: Try=%d - Ack=0x%8x\n",
                trys, force_wake_ack);
    } while (force_wake_ack != 0);

    kprintf("  ACK cleared...\n");

    gfx_outd(pci, FORCE_WAKE_MT, MASKED_ENABLE(1));
    gfx_ind(pci, ECOBUS);

    kprintf("Wake written...\n");
    do {
        ++trys;
        force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK) & 0xFFFF;
        kprintf("Waiting for Force Ack to be Set: Try=%d - Ack=0x%8x\n",
              trys, force_wake_ack);
    } while (force_wake_ack == 0);

    kprintf("...Force Wake done\n");
}

void gfx_exit_force_wake(gfx_pci_t* pci)
{
    gfx_outd(pci, FORCE_WAKE_MT, MASKED_DISABLE(1));
    gfx_ind(pci, ECOBUS);
}

bool pci_get_gfx_device(pci_device_t *gfx_dev)
{
    pci_device_t dev = {0};
    bool found = false;

    /* Find Intel HD graphics device */
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        dev = vec_at(&pci_devices, i); 
        if ((dev.vendor_id != VENDOR_INTEL) ||
            (dev.device_id != DEVICE_HD5500 && dev.device_id != DEVICE_HD520))
        {   
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

    if (gfx_dev != NULL) memcpy(gfx_dev, &dev, sizeof(pci_device_t));
    klogi("PCI: GFX device checking finished.\n");
    return true;
}

bool gfx_init(void)
{
    bool ret = false;
    pci_device_t dev = {0};

    ret = pci_get_gfx_device(&dev);

    if (ret) {
        /* Open and config */
        gfx_init_pci(&gfx_pci, dev);
        gfx_init_gtt(&gfx_pci, &gfx_gtt, dev);
        gfx_init_mem_manager(&gfx_pci, &gfx_gtt, &gfx_mgr);
    }

    return ret;
}

void gfx_mem_enable_swizzle(gfx_pci_t *pci)
{
    /* Only enable swizzling when DIMMs (Dual In-Line Memory Module) are the
     * same size.
     * 15:8 Size of DIMM B in 256 MB multiples
     * 7:0  Size of DIMM A in 256 MB multiples
     */
    uint32_t dimm_ch0 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH0);
    uint32_t dimm_ch1 = gfx_ind(pci, GFX_MCHBAR + MAD_DIMM_CH1);
    kprintf("dimm_ch0: 0x%08x\n", dimm_ch0);
    kprintf("dimm_ch1: 0x%08x\n", dimm_ch1);
    if ((dimm_ch0 & MAD_DIMM_AB_SIZE_MASK) != (dimm_ch1 & MAD_DIMM_AB_SIZE_MASK))
    {
        return;
    }

    /* Enable Bit 6 Swizzling */
    uint32_t arb_ctl = gfx_ind(pci, ARB_CTL);
    arb_ctl |= ARB_CTL_TILED_ADDRESS_SWIZZLING;
    gfx_outd(pci, ARB_CTL, arb_ctl);

    uint32_t tile_ctl = gfx_ind(pci, TILE_CTL);
    tile_ctl |= TILE_CTL_SWIZZLE;
    gfx_outd(pci, TILE_CTL, tile_ctl);

    gfx_outd(pci, ARB_MODE, MASKED_ENABLE(ARB_MODE_AS4TS));

    kprintf("ARB_CTL:  0x%08x\n", gfx_ind(pci, ARB_CTL));
    kprintf("TILE_CTL: 0x%08x\n", gfx_ind(pci, TILE_CTL));
    kprintf("ARB_MODE: 0x%08x\n", gfx_ind(pci, ARB_MODE));
}

uint64_t gfx_addr(gfx_mem_manager_t *mgr, void *phy_addr)
{
    return (uint64_t)((uint8_t*)phy_addr - mgr->gfx_mem_base);
}

bool gfx_alloc(
    gfx_mem_manager_t *mgr, gfx_object_t *obj,
    uint64_t size, uint64_t align)
{
    /* Align memory request */
    volatile uint8_t *cpu_addr = mgr->gfx_mem_next;
    uint64_t offset = (uint64_t)cpu_addr & (align - 1);
    if (offset) {
        cpu_addr += align - offset;
    }

    mgr->gfx_mem_next = cpu_addr + size;
    obj->cpu_addr = cpu_addr;
    obj->gfx_addr = cpu_addr - mgr->gfx_mem_base;

    return true;
}

void gfx_start(void)
{
    bool ret = false;
    pci_device_t dev = {0};

    ret = pci_get_gfx_device(&dev);

    if (ret) {
        kprintf("Found GFX device %2x:%2x.%1x - %4x:%4x %s\n",
                dev.bus, dev.device, dev.func, dev.vendor_id, dev.device_id,
                pci_device_id_to_string(&dev));

        /* We need to force out of D6 state before reading/writing to registers */
        gfx_enter_force_wake(&gfx_pci);
        gfx_mem_enable_swizzle(&gfx_pci);
        gfx_exit_force_wake(&gfx_pci);
    }    
}

