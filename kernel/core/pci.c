#include <stdint.h>

#include <core/pci.h>
#include <core/cpu.h>
#include <core/panic.h>
#include <lib/klog.h>
#include <lib/vector.h>

#define MAX_FUNCTION            8
#define MAX_DEVICE              16

vec_new_static(pci_device_t, pci_devices);

static void pci_scan_bus(uint8_t bus_id);
static void pci_scan_device(uint8_t bus_id, uint8_t dev_id);

static pci_device_desc_t device_table[] =
{
    /* Intel */
    {0x8086, 0x0154, "3rd Gen Core processor DRAM Controller"},
    {0x8086, 0x0166, "3rd Gen Core processor Graphics Controller"},
    {0x8086, 0x100E, "Gigabit Ethernet Controller"},
    {0x8086, 0x153A, "Ethernet Connection I217-LM"},
    {0x8086, 0x10D3, "82574L Gigabit Network Connection"},
    {0x8086, 0x10EA, "82577LM Gigabit Network Connection"},
    {0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI"},
    {0x8086, 0x7000, "82371SB PIIX3 ISA"},
    {0x8086, 0x7010, "82371SB PIIX3 IDE"},
    {0x8086, 0x1237, "440FX - 82441FX PMC"},
    {0x8086, 0x2922, "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller"},
    {0x8086, 0x29C0, "82G33/G31/P35/P31 Express DRAM Controller"},
    /* Realtek */
    {0x10EC, 0x8139, "RTL-8100/8101L/8139 pci Fast Ethernet Adapter"},
    /* QEMU */
    {0x1234, 0x1111, "QEMU Virtual Video Controller"},
    /* VirtualBox */
    {0x80EE, 0xBEEF, "VirtualBox Graphics Adapter"},
    {0x80EE, 0xCAFE, "VirtualBox Guest Service"},
    /* End */
    {0,      0,      "Unknown device"}
};

static const char *pci_device_id_to_string(pci_device_t *device)
{
    for(uint64_t i = 0; ; i++) {
        if (device_table[i].vendor_id == device->vendor_id) {
            if (device_table[i].device_id == device->device_id) {
                return device_table[i].desc;
            }
        }
        /* Reach the last line */
        if (device_table[i].vendor_id == 0) {
            return device_table[i].desc;
        }  
    }
    return NULL;
}

static uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint32_t offset)
{
    uint32_t bus32 = (uint32_t)bus;
    uint32_t device32 = (uint32_t)slot;
    uint32_t func32 = (uint32_t)func;

    uint32_t address = 0x80000000 | (bus32 << 16)
                    | (device32 << 11) 
                    | (func32 << 8)
                    | (offset & 0xFC);

    panic_unless(!(offset & 3));
    port_outd(0xCF8, address);
    return port_ind(0xCFC);
}

#define pci_func_exist(dev)     ((uint16_t)(pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0) & 0xFFFF) != 0xFFFF)

#define pci_read_vendor_id(dev) (pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x00) & 0xFFFF)
#define pci_read_device_id(dev) (pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x00) >> 16)
#define pci_read_class(dev)     (pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x08) >> 24)
#define pci_read_subclass(dev)  ((pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x08) >> 16) & 0xFF)
#define pci_read_prog_if(dev)   ((pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x08) >> 8) & 0xFF)
#define pci_read_header(dev)    (((pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x08) >> 16) & ~(1 << 7)) & 0xFF)
#define pci_read_sub_bus(dev)   ((pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x18) >> 8) & 0xFF)

#define pci_is_bridge(dev)      (pci_read_header(dev) == 0x1 && pci_read_class(dev) == 0x6)
#define pci_has_multi_func(dev) (((pci_read_dword((dev)->bus, (dev)->device, (dev)->func, 0x0C) >> 16) & (1 << 7)) & 0xFF)

static void pci_scan_device(uint8_t bus_id, uint8_t dev_id)
{
    pci_device_t device = {0};
    device.bus = bus_id;
    device.func = 0;
    device.device = dev_id;

    uint8_t func_exist = pci_func_exist(&device);
    uint8_t is_bridge = pci_is_bridge(&device);
    uint8_t has_multi_func = pci_func_exist(&device);

    if (func_exist) {
        if (is_bridge) {
            uint8_t sub_bus_id = pci_read_sub_bus(&device);
            if (sub_bus_id != bus_id) {
                klog_printf("PCI:\tRead sub bus %2x\n", sub_bus_id);
                pci_scan_bus(pci_read_sub_bus(&device));
            }
        }

        device.multifunction = has_multi_func;
        device.device_id = pci_read_device_id(&device);
        device.vendor_id = pci_read_vendor_id(&device);

        klog_printf("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
                    device.bus, device.device, device.func,
                    device.vendor_id, device.device_id,
                    pci_device_id_to_string(&device));
        vec_push_back(&pci_devices, device);

        if (device.multifunction) {
            for (uint8_t func = 1; func < MAX_FUNCTION; func++) {
                pci_device_t device2 = {0};
                device2.bus = bus_id;
                device2.func = func;
                device2.device = dev_id;

                if (pci_func_exist(&device2)) {
                    device2.device_id = pci_read_device_id(&device2);
                    device2.vendor_id = pci_read_vendor_id(&device2);

                    klog_printf("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
                                device2.bus, device2.device, device2.func,
                                device2.vendor_id, device2.device_id,
                                pci_device_id_to_string(&device2));
                    vec_push_back(&pci_devices, device2);
                }
            }
        }
    }
}

static void pci_scan_bus(uint8_t bus_id)
{
    for (uint8_t dev = 0; dev != MAX_DEVICE; dev++) {
        pci_scan_device(bus_id, dev);
    }
}

void pci_init(void)
{
    pci_scan_bus(0);

    klogi("PCI: Full recursive device scan done, [%d] devices found\n", vec_length(&pci_devices));
}

void pci_debug(void)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i); 
        kprintf("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
                dev.bus, dev.device, dev.func, dev.vendor_id, dev.device_id,
                pci_device_id_to_string(&dev));
    } 
}
