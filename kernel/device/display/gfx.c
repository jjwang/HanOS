#include <3rd-party/boot/limine.h>
#include <stdint.h>
#include <sys/cpu.h>
#include <sys/pci.h>
#include <sys/pit.h>
#include <sys/mm.h>
#include <base/vector.h>
#include <base/klog.h>
#include <device/display/gfx.h>
#include <device/display/gfx_reg.h>

#define DEVICE_HD4600           0x0A16
#define DEVICE_PANTHERPOINT     0x1e00

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

void gfx_init_pci(gfx_pci_t* pci, pci_device_t dev,
                  struct limine_kernel_address_response* kernel)
{
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    /* Read PCI registers */
    pci_bar_t bar;

    /* Graphics Memory Address Spaces
     * BAR0: GTTMMADR - The combined Graphics Translation Table Modification
     * Range and Memory Mapped Range. GTTADR will begin at GTTMMADR 2MB while
     * the MMIO base address will be the same as GTTMMADR
     * Ref: https://01.org/sites/default/files/documentation/intel-gfx-prm-
     *      osrc-hsw-pcie-config-registers.pdf#page=129 */
    pci_get_bar(&bar, id, 0);
    pci->mmio_bar = (volatile void*)(kernel->virtual_base + bar.u.address
                    - kernel->physical_base);
    pci->gtt_addr = (volatile uint32_t*)((uint8_t*)pci->mmio_bar + 2 * MB);
    klogi("\tGTTMMADR: 0x%11x (%d MB)\n", bar.u.address, bar.size / MB);

    vmm_map(NULL, (uint64_t)pci->mmio_bar, (uint64_t)bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO);

    /* BAR2: GMADR - Address range allocated via the Device 2 (integrated
     * graphics device) GMADR register. The processor and other peer (DMI)
     * devices utilize this address space to read/write graphics data that
     * resides in Main Memory. This address is internally converted to a
     * GM_Address. */
    pci_get_bar(&bar, id, 2);
    pci->aperture_bar = (volatile void*)(kernel->virtual_base + bar.u.address
                        - kernel->physical_base);
    pci->aperture_size = bar.size;
    klogi("\tGMADR:    0x%11x (%d MB)\n", bar.u.address, bar.size / MB);

    vmm_map(NULL, (uint64_t)pci->aperture_bar, (uint64_t)bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO);

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
    for (size_t fence_num = 0; fence_num < FENCE_COUNT; fence_num++) {
        gfx_outl(pci, FENCE_BASE + sizeof(uint64_t) * fence_num, 0);
    }

    /* TEMP */
    mgr->gfx_mem_base = pci->aperture_bar;
    mgr->gfx_mem_next = mgr->gfx_mem_base + 4 * GTT_PAGE_SIZE;
}

void gfx_disable_vga(gfx_pci_t* pci)
{
    port_outb(SR_INDEX, SEQ_CLOCKING);
    port_outb(SR_DATA, port_inb(SR_DATA) | SCREEN_OFF);
    pit_wait(100);
    gfx_outd(pci, VGA_CONTROL, VGA_DISABLE);

    klogi("VGA Plane disabled\n");
}

void gfx_enter_force_wake(gfx_pci_t* pci)
{
    klogi("Trying to entering force wake...\n");

    int trys = 0;
    int force_wake_ack;
    do {
        ++trys;
        force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK);
        klogd("Waiting for Force Ack to Clear: Try=%d - Ack=0x%8x\n",
              trys, force_wake_ack);
    } while (force_wake_ack != 0);

    klogi("  ACK cleared...\n");

    gfx_outd(pci, FORCE_WAKE_MT, MASKED_ENABLE(1));
    gfx_ind(pci, ECOBUS);

    klogi("Wake written...\n");
    do {
        ++trys;
        force_wake_ack = gfx_ind(pci, FORCE_WAKE_MT_ACK);
        klogd("Waiting for Force Ack to be Set: Try=%d - Ack=0x%8x\n",
              trys, force_wake_ack);
    } while (force_wake_ack == 0);

    klogi("...Force Wake done\n");
}

void gfx_exit_force_wake(gfx_pci_t* pci)
{
    gfx_outd(pci, FORCE_WAKE_MT, MASKED_DISABLE(1));
    gfx_ind(pci, ECOBUS);
}

pci_device_t pci_get_gfx_device(struct limine_kernel_address_response* kernel)
{
    pci_device_t dev = {0};
    bool found = false;

    /* Find Intel HD graphics device */
    for (size_t i = 0; i < vec_length(&pci_devices); i++) {
        dev = vec_at(&pci_devices, i); 
        if ((dev.vendor_id != VENDOR_INTEL) ||
            (dev.device_id != DEVICE_HD4600))
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
        return dev;
    }   

    /* Open and config */
    gfx_pci_t pci;
    gfx_gtt_t gtt;
    gfx_mem_manager_t mgr;

    gfx_init_pci(&pci, dev, kernel);
    gfx_init_gtt(&pci, &gtt, dev);
    gfx_init_mem_manager(&pci, &gtt, &mgr);

    klogi("PCI: GFX device checking finished.\n");
    return dev;
}
