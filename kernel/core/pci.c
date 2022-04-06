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

#define ID_DEVICE_8086_100E     0
#define ID_DEVICE_8086_153A     1
#define ID_DEVICE_8086_10EA     2
#define ID_DEVICE_8086_7113     3
#define ID_DEVICE_8086_2922     4
#define ID_DEVICE_8086_7000     5
#define ID_DEVICE_8086_7010     6
#define ID_DEVICE_8086_1237     7
#define ID_DEVICE_10EC_8139     10
#define ID_DEVICE_1234_1111     20
#define ID_DEVICE_80EE_BEEF     30
#define ID_DEVICE_80EE_CAFE     31
#define ID_DEVICE_DEFAULT       99

static const char* const device_name_table[] = 
{
    [ID_DEVICE_8086_100E] = "Gigabit Ethernet Controller",
    [ID_DEVICE_8086_153A] = "Ethernet Connection I217-LM",
    [ID_DEVICE_8086_10EA] = "Gigabit Network Connection",
    [ID_DEVICE_8086_7113] = "82371AB/EB/MB PIIX4 ACPI",
    [ID_DEVICE_8086_7000] = "82371SB PIIX3 ISA",
    [ID_DEVICE_8086_7010] = "82371SB PIIX3 IDE",
    [ID_DEVICE_8086_1237] = "440FX - 82441FX PMC",
    [ID_DEVICE_8086_2922] = "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller",
    [ID_DEVICE_10EC_8139] = "RTL-8100/8101L/8139 pci Fast Ethernet Adapter",
    [ID_DEVICE_1234_1111] = "QEMU Virtual Video Controller",
    [ID_DEVICE_80EE_BEEF] = "VirtualBox Graphics Adapter",
    [ID_DEVICE_80EE_CAFE] = "VirtualBox Guest Service",
    [ID_DEVICE_DEFAULT]   = "Unknown device"
};

static const char *pci_device_id_to_string(pci_device_t *device)
{
    switch (device->vendor_id) {
    /* Intel */
    case 0x8086:
        switch (device->device_id) {
        case 0x100E:
            return device_name_table[ID_DEVICE_8086_100E]; /* "Gigabit Ethernet Controller" */
        case 0x153A:
            return device_name_table[ID_DEVICE_8086_153A]; /* "Ethernet Connection I217-LM" */
        case 0x10EA:
            return device_name_table[ID_DEVICE_8086_10EA]; /* "Gigabit Network Connection" */
        case 0x7113:
            return device_name_table[ID_DEVICE_8086_7113]; /* "82371AB/EB/MB PIIX4 ACPI" */
        case 0x2922:
            return device_name_table[ID_DEVICE_8086_2922]; /* "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller" */
        case 0x7000:
            return device_name_table[ID_DEVICE_8086_7000]; /* "82371SB PIIX3 ISA" */
        case 0x7010:
            return device_name_table[ID_DEVICE_8086_7010]; /* "82371SB PIIX3 IDE" */
        case 0x1237:
            return device_name_table[ID_DEVICE_8086_1237]; /* "440FX - 82441FX PMC" */
        }
        break;
    /* Realtek */
    case 0x10EC:
        switch (device->device_id) {
        case 0x8139:
            return device_name_table[ID_DEVICE_10EC_8139]; /* "RTL-8100/8101L/8139 PCI Fast Ethernet Adapter" */
        }
        break;
    /* QEMU */
    case 0x1234:
        switch (device->device_id) {
        case 0x1111:
            return device_name_table[ID_DEVICE_1234_1111]; /* "QEMU Virtual Video Controller" */
        }
        break;
    /* VirtualBox */
    case 0x80EE:
        switch (device->device_id) {
        case 0xBEEF:
            return device_name_table[ID_DEVICE_80EE_BEEF]; /* "VirtualBox Graphics Adapter" */
        case 0xCAFE:
            return device_name_table[ID_DEVICE_80EE_CAFE]; /* "VirtualBox Guest Service" */
        }
        break;
    }
    return device_name_table[ID_DEVICE_DEFAULT];           /* "Unknown device" */
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

    klogi("PCI: Full recursive device scan done, [%d] devices found\n", pci_devices.len);
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
