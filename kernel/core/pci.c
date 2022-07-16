/**-----------------------------------------------------------------------------

 @file    pci.c
 @brief   Implementation of PCI related functions
 @details
 @verbatim

  There are two functions in this file:
  1. pci_init() which can scan all PCI devices and store information into an
     array.
  2. pci_debug() which can be called by command line and display PCI device
     list.

 @endverbatim
  Ref: https://wiki.osdev.org/PCI

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <core/pci.h>
#include <core/cpu.h>
#include <core/panic.h>
#include <lib/klog.h>
#include <lib/vector.h>

#define MAX_FUNCTION            8
#define MAX_DEVICE              16

vec_new(pci_device_t, pci_devices);

static void pci_scan_bus(uint8_t bus_id);
static void pci_scan_device(uint8_t bus_id, uint8_t dev_id);

static pci_device_desc_t device_table[] =
{
    /* Intel */
    {0x8086, 0x0154, "3rd Gen Core processor DRAM Controller"},
    {0x8086, 0x0166, "3rd Gen Core processor Graphics Controller"},
    {0x8086, 0x100E, "Gigabit Ethernet Controller"},
    {0x8086, 0x0A04, "Haswell-ULT DRAM Controller"},
    {0x8086, 0x0A0C, "Haswell-ULT HD Audio Controller"},
    {0x8086, 0x0A16, "Haswell-ULT Integrated Graphics Controller"},
    {0x8086, 0x153A, "Ethernet Connection I217-LM"},
    {0x8086, 0x10D3, "82574L Gigabit Network Connection"},
    {0x8086, 0x10EA, "82577LM Gigabit Network Connection"},
    {0x8086, 0x7000, "82371SB PIIX3 ISA"},
    {0x8086, 0x7010, "82371SB PIIX3 IDE"},
    {0x8086, 0x7110, "82371AB/EB/MB PIIX4 ISA"},
    {0x8086, 0x7111, "82371AB/EB/MB PIIX4 IDE"},
    {0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI"},
    {0x8086, 0x7192, "440BX/ZX/DX - 82443BX/ZX/DX Host bridge (AGP disabled)"},
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
    /* Hyper-V */
    {0x1414, 0x5353, "Hyper-V virtual VGA"},
    /* End */
    {0,      0,      "Unknown device"}
};

static const char unknown_device_desc[] = "Unknown device";

const char *pci_device_id_to_string(pci_device_t *device)
{
    for(uint64_t i = 0; ; i++) {
        /* Reach the last line */
        if (device_table[i].vendor_id == 0) {
            return unknown_device_desc;
        }
        if (device_table[i].vendor_id == device->vendor_id) {
            if (device_table[i].device_id == device->device_id) {
                return device_table[i].desc;
            }
        }
    }
    return unknown_device_desc;
}

static void pci_read_bar(
    uint32_t id, uint32_t index, uint32_t *address, uint32_t *mask)
{
    uint32_t reg = PCI_CONFIG_BAR0 + index * sizeof(uint32_t);

    /* Get address */
    *address = pci_ind(id, reg);

    /* Find out size of the bar */
    pci_outd(id, reg, 0xffffffff);
    *mask = pci_ind(id, reg);

    /* Restore adddress */
    pci_outd(id, reg, *address);
}

void pci_get_bar(pci_bar_t *bar, uint32_t id, uint32_t index)
{
    /* Read pci bar register */
    uint32_t addr_low;
    uint32_t mask_low;
    pci_read_bar(id, index, &addr_low, &mask_low);

    if (addr_low & PCI_BAR_64) {
        /* 64-bit mmio */
        uint32_t addr_high;
        uint32_t mask_high;
        pci_read_bar(id, index + 1, &addr_high, &mask_high);

        bar->u.address =
            (void *)(((uintptr_t)addr_high << 32) | (addr_low & ~0xf));
        bar->size = ~(((uint64_t)mask_high << 32) | (mask_low & ~0xf)) + 1;
        bar->flags = addr_low & 0xf;
    } else if (addr_low & PCI_BAR_IO) {
        /* I/O register */
        bar->u.port = (uint16_t)(addr_low & ~0x3);
        bar->size = (uint16_t)(~(mask_low & ~0x3) + 1);
        bar->flags = addr_low & 0x3;
    } else {
        /* 32-bit mmio */
        bar->u.address = (void *)(uintptr_t)(addr_low & ~0xf);
        bar->size = ~(mask_low & ~0xf) + 1;
        bar->flags = addr_low & 0xf;
    }
}

uint8_t pci_inb(uint32_t id, uint32_t offset)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);

    port_outd(PCI_PORT_ADDR, address);
    return port_inb(PCI_PORT_DATA + (offset & 0x03));
}

void pci_outb(uint32_t id, uint32_t offset, uint8_t data)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);

    port_outd(PCI_PORT_ADDR, address);
    port_outb(PCI_PORT_DATA + (offset & 0x03), data);
}

uint16_t pci_inw(uint32_t id, uint32_t offset)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);

    port_outd(PCI_PORT_ADDR, address);
    return port_inw(PCI_PORT_DATA + (offset & 0x02));
}   

void pci_outw(uint32_t id, uint32_t offset, uint16_t data)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);
    
    port_outd(PCI_PORT_ADDR, address);
    port_outw(PCI_PORT_DATA + (offset & 0x02), data);
}   

uint32_t pci_ind(uint32_t id, uint32_t offset)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);

    port_outd(PCI_PORT_ADDR, address);
    return port_ind(PCI_PORT_DATA);
}

void pci_outd(uint32_t id, uint32_t offset, uint32_t data)
{
    uint32_t address = 0x80000000 | id | (offset & 0xFC);

    port_outd(PCI_PORT_ADDR, address);
    port_outd(PCI_PORT_DATA, data);
}

#define pci_func_exist(dev)     \
    ((uint16_t)(pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_LEGACY) & 0xFFFF) \
    != 0xFFFF)

#define pci_read_vendor_id(dev) \
    (pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_LEGACY) & 0xFFFF)

#define pci_read_device_id(dev) \
    (pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_LEGACY) >> 16)

#define pci_read_class(dev)     \
    (pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_PERIHPERALS) >> 24)

#define pci_read_subclass(dev)  \
    ((pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_PERIHPERALS) >> 16) & 0xFF)

#define pci_read_prog_if(dev)   \
    ((pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_PERIHPERALS) >> 8) & 0xFF)

#define pci_read_header(dev)    \
    (((pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_PERIHPERALS) >> 16) & ~(1 << 7)) & 0xFF)

#define pci_read_sub_bus(dev)   \
    ((pci_ind(PCI_MAKE_DEVICE_ID(dev), 0x18) >> 8) & 0xFF)

#define pci_is_bridge(dev)      \
    (pci_read_header(dev) == 0x1 && pci_read_class(dev) == 0x6)

#define pci_has_multi_func(dev) \
    (((pci_ind(PCI_MAKE_DEVICE_ID(dev), PCI_CLASS_SERIAL_BUS) >> 16) & (1 << 7)) & 0xFF)

static void pci_scan_device(uint8_t bus_id, uint8_t dev_id)
{
    pci_device_t device = {0};
    device.bus = bus_id;
    device.func = 0;
    device.device = dev_id;

    uint8_t func_exist = pci_func_exist(&device);
    uint8_t is_bridge = pci_is_bridge(&device);
    uint8_t has_multi_func = pci_func_exist(&device);

    if (is_bridge) {
        klogi("PCI:\t%2x:%2x.%1x - %4x:%4x [bridge] func %s\n",
              device.bus, device.device, device.func,
              device.vendor_id, device.device_id,
              func_exist ? "existed" : "not existed");
    }

    if (func_exist) {
        if (is_bridge) {
            uint8_t sub_bus_id = pci_read_sub_bus(&device);
            if (sub_bus_id != bus_id) {
                klogi("PCI:\tRead sub bus %2x\n", sub_bus_id);
                pci_scan_bus(pci_read_sub_bus(&device));
            }
        }

        device.multifunction = has_multi_func;
        device.device_id = pci_read_device_id(&device);
        device.vendor_id = pci_read_vendor_id(&device);

        klogi("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
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

                    klogi("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
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
    for (size_t dev = 0; dev != MAX_DEVICE; dev++) {
        pci_scan_device(bus_id, dev);
    }
}

void pci_init(void)
{
    pci_scan_bus(0);

    klogi("PCI: Full recursive device scan done, [%d] devices found\n",
          vec_length(&pci_devices));
}

void pci_debug(void)
{
    for (size_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i); 
        kprintf("PCI:\t%2x:%2x.%1x - %4x:%4x %s\n",
                dev.bus, dev.device, dev.func, dev.vendor_id, dev.device_id,
                pci_device_id_to_string(&dev));
    } 
}

