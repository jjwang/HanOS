/**-----------------------------------------------------------------------------

 @file    pci.c
 @brief   Implementation of PCI related functions
 @details
 @verbatim

  There are two functions in this file:
  1. pci_init() which can scan all PCI devices and store information into an
     array.
  2. pci_list() which can be called by command line and display PCI device
     list.

  The PCI device can be determined by vendor id and device id:
     https://devicehunt.com/view/type/pci/vendor/1052#search-results-table

 @endverbatim

  Ref: https://wiki.osdev.org/PCI for technical document
       https://devicehunt.com/view/type/pci/vendor/8086 for device name

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <arch/x64/pci.h>
#include <arch/x64/cpu.h>
#include <arch/x64/panic.h>
#include <lib/klog.h>
#include <lib/vector.h>

#define MAX_BUS                 1
#define MAX_DEVICE              32
#define MAX_FUNCTION            8

vec_new(pci_device_t, pci_devices);

static void pci_scan_bus(uint8_t bus_id);
static void pci_scan_device(uint8_t bus_id, uint8_t dev_id);

static pci_device_desc_t device_table[] = {
    /* Intel */
    { 0x8086, 0x0154, "3rd Gen Core processor DRAM Controller" },
    { 0x8086, 0x0166, "3rd Gen Core processor Graphics Controller" },
    { 0x8086, 0x0A04, "Haswell-ULT DRAM Controller" },
    { 0x8086, 0x0A0C, "Haswell-ULT HD Audio Controller" },
    { 0x8086, 0x0A16, "Haswell-ULT GT2 [HD Graphics 4400]" },
    { 0x8086, 0x153A, "Ethernet Connection I217-LM" },
    { 0x8086, 0x1559, "Ethernet Connection I218-V" },
    { 0x8086, 0x155A, "Ethernet Connection I218-LM" },
    { 0x8086, 0x15A0, "Ethernet Connection (2) I218-LM" },
    { 0x8086, 0x15A1, "Ethernet Connection (2) I218-V" },
    { 0x8086, 0x15A2, "Ethernet Connection (3) I218-LM" },
    { 0x8086, 0x15A3, "Ethernet Connection (3) I218-V" },
    { 0x8086, 0x100E, "Gigabit Ethernet Controller" },
    { 0x8086, 0x10D3, "82574L Gigabit Network Connection" },
    { 0x8086, 0x10EA, "82577LM Gigabit Network Connection" },
    { 0x8086, 0x1237, "440FX - 82441FX PMC" },
    { 0x8086, 0x1570, "Ethernet Connection I219-V" },
    { 0x8086, 0x1604, "Broadwell-U Host Bridge -OPI" },
    { 0x8086, 0x160C, "Broadwell-U Audio Controller" },
    { 0x8086, 0x1606, "Broadwell-U GT1 [HD Graphics]" },
    { 0x8086, 0x1616, "Broadwell-U GT2 [HD Graphics 5500]" },
    { 0x8086, 0x161E, "Broadwell-U GT3 [Iris Graphics 6100]" },
    { 0x8086, 0x9C83, "Wildcat Point-LP SATA Controller [AHCI mode]" },
    { 0x8086, 0x9C94, "Wildcat Point-LP PCI Express Root Port #3" },
    { 0x8086, 0x9C9A, "Wildcat Point-LP PCI Express Root Port #6" },
    { 0x8086, 0x9CA0, "Wildcat Point-LP High Definition Audio Controller" },
    { 0x8086, 0x9CA2, "Wildcat Point-LP SMBus Controller" },
    { 0x8086, 0x9CA4, "Wildcat Point-LP Thermal Management Controller" },
    { 0x8086, 0x9CA6, "Wildcat Point-LP USB EHCI Controller" },
    { 0x8086, 0x9CB1, "Wildcat Point-LP USB xHCI Controller" },
    { 0x8086, 0x9CBA, "Wildcat Point-LP MEI Controller #1" },
    { 0x8086, 0x9CC3, "Wildcat Point-LP LPC Controller" },
    { 0x8086, 0x1904,
     "Xeon E3-1200 v5/E3-1500 v5/6th Gen Core Processor Host Bridge/DRAM Registers"
     },
    { 0x8086, 0x1911,
     "Xeon E3-1200 v5/v6 / E3-1500 v5 / 6th/7th/8th Gen Core Processor Gaussian Mixture Model"
     },
    { 0x8086, 0x1916, "Skylake GT2 [HD Graphics 520]" },
    { 0x8086, 0x9D2F, "Sunrise Point-LP USB 3.0 xHCI Controller" },
    { 0x8086, 0x9D03, "Sunrise Point-LP SATA Controller [AHCI mode]" },
    { 0x8086, 0x9D13, "Sunrise Point-LP PCI Express Root Port #4" },
    { 0x8086, 0x9D14, "Sunrise Point-LP PCI Express Root Port #5" },
    { 0x8086, 0x9D18, "Sunrise Point-LP PCI Express Root Port #9" },
    { 0x8086, 0x9D31, "Sunrise Point-LP Thermal subsystem" },
    { 0x8086, 0x9D3A, "Sunrise Point-LP CSME HECI #1" },
    { 0x8086, 0x9D21, "Sunrise Point-LP PMC, " },
    { 0x8086, 0x9D23, "Sunrise Point-LP SMBus" },
    { 0x8086, 0x9D48, "Sunrise Point-LP LPC Controller" },
    { 0x8086, 0x9D70, "Sunrise Point-LP HD Audio" },
    /* Kaby Lake, Sunrise Point and 200 Series PCH */
    { 0x8086, 0x15D6, "Ethernet Connection (5) I219-V" },
    { 0x8086, 0x15D7, "Ethernet Connection (4) I219-LM" },
    { 0x8086, 0x15D8, "Ethernet Connection (4) I219-V" },
    { 0x8086, 0x15D9, "JHL6340 Thunderbolt 3 NHI (C step) [Alpine Ridge 2C 2016]" },
    { 0x8086, 0x15DA, "JHL6340 Thunderbolt 3 Bridge (C step) [Alpine Ridge 2C 2016]" },
    { 0x8086, 0x15DB, "JHL6340 Thunderbolt 3 USB 3.1 Controller (C step) [Alpine Ridge 2C 2016]" },
    { 0x8086, 0x15DF, "Ethernet Connection (8) I219-LM" },
    { 0x8086, 0x15E0, "Ethernet Connection (8) I219-V" },
    { 0x8086, 0x15E1, "Ethernet Connection (9) I219-LM" },
    { 0x8086, 0x15E2, "Ethernet Connection (9) I219-V" },
    { 0x8086, 0x15E3, "Ethernet Connection (5) I219-LM" },
    { 0x8086, 0x5900, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5901, "Kaby Lake PCI Express Controller (x16)" },
    { 0x8086, 0x5902, "Kaby Lake GT1 [HD Graphics 610]" },
    { 0x8086, 0x5904, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5905, "Kaby Lake PCI Express Controller (x8)" },
    { 0x8086, 0x5909, "Kaby Lake PCI Express Controller (x4)" },
    { 0x8086, 0x590C, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x590F, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5910, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5911, "Kaby Lake Gaussian Mixture Model" },
    { 0x8086, 0x5912, "Kaby Lake GT2 [HD Graphics 630]" },
    { 0x8086, 0x5914, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5916, "Kaby Lake GT2 [HD Graphics 620]" },
    { 0x8086, 0x5917, "Kaby Lake GT2 [UHD Graphics 620]" },
    { 0x8086, 0x5918, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x591B, "Kaby Lake GT2 [HD Graphics 630]" },
    { 0x8086, 0x591C, "Kaby Lake GT2 [UHD Graphics 615]" },
    { 0x8086, 0x591D, "Kaby Lake GT2 [HD Graphics P630]" },
    { 0x8086, 0x591E, "Kaby Lake GT2 [HD Graphics 615]" },
    { 0x8086, 0x591F, "Kaby Lake Host Bridge/DRAM Registers" },
    { 0x8086, 0x5923, "Kaby Lake GT3e [HD Graphics 635]" },
    { 0x8086, 0x5926, "Kaby Lake GT3e [Iris Plus Graphics 640]" },
    { 0x8086, 0x5927, "Kaby Lake GT3e [Iris Plus Graphics 650]" },
    { 0x8086, 0x9D10, "Sunrise Point-LP PCI Express Root Port #1" },
    { 0x8086, 0x9D11, "Sunrise Point-LP PCI Express Root Port #2" },
    { 0x8086, 0x9D12, "Sunrise Point-LP PCI Express Root Port #3" },
    { 0x8086, 0x9D15, "Sunrise Point-LP PCI Express Root Port #6" },
    { 0x8086, 0x9D16, "Sunrise Point-LP PCI Express Root Port #7" },
    { 0x8086, 0x9D17, "Sunrise Point-LP PCI Express Root Port #8" },
    { 0x8086, 0x9D19, "Sunrise Point-LP PCI Express Root Port #10" },
    { 0x8086, 0x9D1A, "Sunrise Point-LP PCI Express Root Port #11" },
    { 0x8086, 0x9D27, "Sunrise Point-LP Serial IO UART Controller #0" },
    { 0x8086, 0x9D28, "Sunrise Point-LP Serial IO UART Controller #1" },
    { 0x8086, 0x9D29, "Sunrise Point-LP Serial IO SPI Controller #0" },
    { 0x8086, 0x9D2A, "Sunrise Point-LP Serial IO SPI Controller #1" },
    { 0x8086, 0x9D2D, "Sunrise Point-LP Secure Digital IO Controller" },
    { 0x8086, 0x9D35, "Sunrise Point-LP Integrated Sensor Hub" },
    { 0x8086, 0x9D3D, "Sunrise Point-LP Active Management Technology - SOL" },
    { 0x8086, 0x9D43, "Sunrise Point-LP LPC Controller" },
    { 0x8086, 0x9D4E, "Sunrise Point LPC Controller/eSPI Controller" },
    { 0x8086, 0x9D50, "Sunrise Point LPC Controller" },
    { 0x8086, 0x9D56, "Sunrise Point-LP LPC Controller" },
    { 0x8086, 0x9D58, "Sunrise Point-LP LPC Controller" },
    { 0x8086, 0x9D60, "Sunrise Point-LP Serial IO I2C Controller #0" },
    { 0x8086, 0x9D61, "Sunrise Point-LP Serial IO I2C Controller #1" },
    { 0x8086, 0x9D62, "Sunrise Point-LP Serial IO I2C Controller #2" },
    { 0x8086, 0x9D63, "Sunrise Point-LP Serial IO I2C Controller #3" },
    { 0x8086, 0x9D64, "Sunrise Point-LP Serial IO I2C Controller #4" },
    { 0x8086, 0x9D65, "Sunrise Point-LP Serial IO I2C Controller #5" },
    { 0x8086, 0x9D66, "Sunrise Point-LP Serial IO UART Controller #2" },
    { 0x8086, 0x9D71, "Sunrise Point-LP HD Audio" },
    { 0x8086, 0xA102, "Q170/Q150/B150/H170/H110/Z170/CM236 Chipset SATA Controller [AHCI Mode]" },
    { 0x8086, 0xA103, "HM170/QM170 Chipset SATA Controller [AHCI Mode]" },
    { 0x8086, 0xA105, "Sunrise Point-H SATA Controller [RAID mode]" },
    { 0x8086, 0xA106, "Q170/H170/Z170/CM236 Chipset SATA Controller [RAID Mode]" },
    { 0x8086, 0xA107, "HM170/QM170 Chipset SATA Controller [RAID Mode]" },
    { 0x8086, 0xA10F, "Sunrise Point-H SATA Controller [RAID mode]" },
    { 0x8086, 0xA110, "100 Series/C230 Series Chipset Family PCI Express Root Port #1" },
    { 0x8086, 0xA111, "100 Series/C230 Series Chipset Family PCI Express Root Port #2" },
    { 0x8086, 0xA112, "100 Series/C230 Series Chipset Family PCI Express Root Port #3" },
    { 0x8086, 0xA113, "100 Series/C230 Series Chipset Family PCI Express Root Port #4" },
    { 0x8086, 0xA114, "100 Series/C230 Series Chipset Family PCI Express Root Port #5" },
    { 0x8086, 0xA115, "100 Series/C230 Series Chipset Family PCI Express Root Port #6" },
    { 0x8086, 0xA116, "100 Series/C230 Series Chipset Family PCI Express Root Port #7" },
    { 0x8086, 0xA117, "100 Series/C230 Series Chipset Family PCI Express Root Port #8" },
    { 0x8086, 0xA118, "100 Series/C230 Series Chipset Family PCI Express Root Port #9" },
    { 0x8086, 0xA119, "100 Series/C230 Series Chipset Family PCI Express Root Port #10" },
    { 0x8086, 0xA11A, "100 Series/C230 Series Chipset Family PCI Express Root Port #11" },
    { 0x8086, 0xA11B, "100 Series/C230 Series Chipset Family PCI Express Root Port #12" },
    { 0x8086, 0xA11C, "100 Series/C230 Series Chipset Family PCI Express Root Port #13" },
    { 0x8086, 0xA11D, "100 Series/C230 Series Chipset Family PCI Express Root Port #14" },
    { 0x8086, 0xA11E, "100 Series/C230 Series Chipset Family PCI Express Root Port #15" },
    { 0x8086, 0xA11F, "100 Series/C230 Series Chipset Family PCI Express Root Port #16" },
    { 0x8086, 0xA120, "100 Series/C230 Series Chipset Family P2SB" },
    { 0x8086, 0xA121, "100 Series/C230 Series Chipset Family Power Management Controller" },
    { 0x8086, 0xA122, "Sunrise Point-H cAVS" },
    { 0x8086, 0xA123, "100 Series/C230 Series Chipset Family SMBus" },
    { 0x8086, 0xA124, "100 Series/C230 Series Chipset Family SPI Controller" },
    { 0x8086, 0xA125, "100 Series/C230 Series Chipset Family Gigabit Ethernet Controller" },
    { 0x8086, 0xA126, "100 Series/C230 Series Chipset Family Trace Hub" },
    { 0x8086, 0xA127, "100 Series/C230 Series Chipset Family Serial IO UART #0" },
    { 0x8086, 0xA128, "100 Series/C230 Series Chipset Family Serial IO UART #1" },
    { 0x8086, 0xA129, "100 Series/C230 Series Chipset Family Serial IO GSPI #0" },
    { 0x8086, 0xA12A, "100 Series/C230 Series Chipset Family Serial IO GSPI #1" },
    { 0x8086, 0xA12F, "100 Series/C230 Series Chipset Family USB 3.0 xHCI Controller" },
    { 0x8086, 0xA130, "100 Series/C230 Series Chipset Family USB Device Controller (OTG)" },
    { 0x8086, 0xA131, "100 Series/C230 Series Chipset Family Thermal Subsystem" },
    { 0x8086, 0xA133, "Sunrise Point-H Northpeak ACPI Function" },
    { 0x8086, 0xA135, "100 Series/C230 Series Chipset Family Integrated Sensor Hub" },
    { 0x8086, 0xA13A, "100 Series/C230 Series Chipset Family MEI Controller #1" },
    { 0x8086, 0xA13B, "100 Series/C230 Series Chipset Family MEI Controller #2" },
    { 0x8086, 0xA13C, "100 Series/C230 Series Chipset Family IDE Redirection" },
    { 0x8086, 0xA13D, "100 Series/C230 Series Chipset Family KT Redirection" },
    { 0x8086, 0xA13E, "100 Series/C230 Series Chipset Family MEI Controller #3" },
    { 0x8086, 0xA140, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA141, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA142, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA143, "H110 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA144, "H170 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA145, "Z170 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA146, "Q170 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA147, "Q150 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA148, "B150 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA149, "C236 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA14A, "C232 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA14B, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA14C, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA14D, "QM170 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA14E, "HM170 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA14F, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA150, "CM236 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA151, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA152, "HM175 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA153, "QM175 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA154, "CM238 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA155, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA156, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA157, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA158, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA159, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15A, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15B, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15C, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15D, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15E, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA15F, "Sunrise Point-H LPC Controller" },
    { 0x8086, 0xA160, "100 Series/C230 Series Chipset Family Serial IO I2C Controller #0" },
    { 0x8086, 0xA161, "100 Series/C230 Series Chipset Family Serial IO I2C Controller #1" },
    { 0x8086, 0xA162, "100 Series/C230 Series Chipset Family Serial IO I2C Controller #2" },
    { 0x8086, 0xA163, "100 Series/C230 Series Chipset Family Serial IO I2C Controller #3" },
    { 0x8086, 0xA166, "100 Series/C230 Series Chipset Family Serial IO UART Controller #2" },
    { 0x8086, 0xA167, "100 Series/C230 Series Chipset Family PCI Express Root Port #17" },
    { 0x8086, 0xA168, "100 Series/C230 Series Chipset Family PCI Express Root Port #18" },
    { 0x8086, 0xA169, "100 Series/C230 Series Chipset Family PCI Express Root Port #19" },
    { 0x8086, 0xA16A, "100 Series/C230 Series Chipset Family PCI Express Root Port #20" },
    { 0x8086, 0xA170, "100 Series/C230 Series Chipset Family HD Audio Controller" },
    { 0x8086, 0xA171, "CM238 HD Audio Controller" },
    { 0x8086, 0xA182, "C620 Series Chipset Family SATA Controller [AHCI mode]" },
    { 0x8086, 0xA186, "C620 Series Chipset Family SATA Controller [RAID mode]" },
    { 0x8086, 0xA190, "C620 Series Chipset Family PCI Express Root Port #1" },
    { 0x8086, 0xA191, "C620 Series Chipset Family PCI Express Root Port #2" },
    { 0x8086, 0xA192, "C620 Series Chipset Family PCI Express Root Port #3" },
    { 0x8086, 0xA193, "C620 Series Chipset Family PCI Express Root Port #4" },
    { 0x8086, 0xA194, "C620 Series Chipset Family PCI Express Root Port #5" },
    { 0x8086, 0xA195, "C620 Series Chipset Family PCI Express Root Port #6" },
    { 0x8086, 0xA196, "C620 Series Chipset Family PCI Express Root Port #7" },
    { 0x8086, 0xA197, "C620 Series Chipset Family PCI Express Root Port #8" },
    { 0x8086, 0xA198, "C620 Series Chipset Family PCI Express Root Port #9" },
    { 0x8086, 0xA199, "C620 Series Chipset Family PCI Express Root Port #10" },
    { 0x8086, 0xA19A, "C620 Series Chipset Family PCI Express Root Port #11" },
    { 0x8086, 0xA19B, "C620 Series Chipset Family PCI Express Root Port #12" },
    { 0x8086, 0xA19C, "C620 Series Chipset Family PCI Express Root Port #13" },
    { 0x8086, 0xA19D, "C620 Series Chipset Family PCI Express Root Port #14" },
    { 0x8086, 0xA19E, "C620 Series Chipset Family PCI Express Root Port #15" },
    { 0x8086, 0xA19F, "C620 Series Chipset Family PCI Express Root Port #16" },
    { 0x8086, 0xA1A0, "C620 Series Chipset Family P2SB" },
    { 0x8086, 0xA1A1, "C620 Series Chipset Family Power Management Controller" },
    { 0x8086, 0xA1A2, "C620 Series Chipset Family cAVS" },
    { 0x8086, 0xA1A3, "C620 Series Chipset Family SMBus" },
    { 0x8086, 0xA1A4, "C620 Series Chipset Family SPI Controller" },
    { 0x8086, 0xA1A6, "C620 Series Chipset Family Trace Hub" },
    { 0x8086, 0xA1AF, "C620 Series Chipset Family USB 3.0 xHCI Controller" },
    { 0x8086, 0xA1B1, "C620 Series Chipset Family Thermal Subsystem" },
    { 0x8086, 0xA1BA, "C620 Series Chipset Family MEI Controller #1" },
    { 0x8086, 0xA1BB, "C620 Series Chipset Family MEI Controller #2" },
    { 0x8086, 0xA1BC, "C620 Series Chipset Family IDE Redirection" },
    { 0x8086, 0xA1BD, "C620 Series Chipset Family KT Redirection" },
    { 0x8086, 0xA1BE, "C620 Series Chipset Family MEI Controller #3" },
    { 0x8086, 0xA1C1, "C621 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C2, "C622 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C3, "C624 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C4, "C625 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C5, "C626 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C6, "C627 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1C7, "C628 Series Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA1D2, "C620 Series Chipset Family SSATA Controller [AHCI mode]" },
    { 0x8086, 0xA1D6, "C620 Series Chipset Family SSATA Controller [RAID mode]" },
    { 0x8086, 0xA1E7, "C620 Series Chipset Family PCI Express Root Port #17" },
    { 0x8086, 0xA1E8, "C620 Series Chipset Family PCI Express Root Port #18" },
    { 0x8086, 0xA1E9, "C620 Series Chipset Family PCI Express Root Port #19" },
    { 0x8086, 0xA1EA, "C620 Series Chipset Family PCI Express Root Port #20" },
    { 0x8086, 0xA1EC, "C620 Series Chipset Family MROM 0" },
    { 0x8086, 0xA1ED, "C620 Series Chipset Family MROM 1" },
    { 0x8086, 0xA1F8, "Lewisburg IE: HECI #1" },
    { 0x8086, 0xA1F9, "Lewisburg IE: HECI #2" },
    { 0x8086, 0xA1FA, "Lewisburg IE: IDE-r" },
    { 0x8086, 0xA1FB, "Lewisburg IE: KT Controller" },
    { 0x8086, 0xA1FC, "Lewisburg IE: HECI #3" },
    { 0x8086, 0xA202, "Lewisburg SATA Controller [AHCI mode]" },
    { 0x8086, 0xA206, "Lewisburg SATA Controller [RAID mode]" },
    { 0x8086, 0xA223, "Lewisburg SMBus" },
    { 0x8086, 0xA224, "Lewisburg SPI Controller" },
    { 0x8086, 0xA242, "Lewisburg LPC or eSPI Controller" },
    { 0x8086, 0xA243, "Lewisburg LPC or eSPI Controller" },
    { 0x8086, 0xA252, "Lewisburg SSATA Controller [AHCI mode]" },
    { 0x8086, 0xA256, "Lewisburg SSATA Controller [RAID mode]" },
    { 0x8086, 0xA282, "200 Series PCH SATA controller [AHCI mode]" },
    { 0x8086, 0xA286, "200 Series PCH SATA controller [RAID mode]" },
    { 0x8086, 0xA290, "200 Series PCH PCI Express Root Port #1" },
    { 0x8086, 0xA291, "200 Series PCH PCI Express Root Port #2" },
    { 0x8086, 0xA292, "200 Series PCH PCI Express Root Port #3" },
    { 0x8086, 0xA293, "200 Series PCH PCI Express Root Port #4" },
    { 0x8086, 0xA294, "200 Series PCH PCI Express Root Port #5" },
    { 0x8086, 0xA295, "200 Series PCH PCI Express Root Port #6" },
    { 0x8086, 0xA296, "200 Series PCH PCI Express Root Port #7" },
    { 0x8086, 0xA297, "200 Series PCH PCI Express Root Port #8" },
    { 0x8086, 0xA298, "200 Series PCH PCI Express Root Port #9" },
    { 0x8086, 0xA299, "200 Series PCH PCI Express Root Port #10" },
    { 0x8086, 0xA29A, "200 Series PCH PCI Express Root Port #11" },
    { 0x8086, 0xA29B, "200 Series PCH PCI Express Root Port #12" },
    { 0x8086, 0xA29C, "200 Series PCH PCI Express Root Port #13" },
    { 0x8086, 0xA29D, "200 Series PCH PCI Express Root Port #14" },
    { 0x8086, 0xA29E, "200 Series PCH PCI Express Root Port #15" },
    { 0x8086, 0xA29F, "200 Series PCH PCI Express Root Port #16" },
    { 0x8086, 0xA2A0, "200 Series/Z370 Chipset Family P2SB" },
    { 0x8086, 0xA2A1, "200 Series/Z370 Chipset Family Power Management Controller" },
    { 0x8086, 0xA2A3, "200 Series/Z370 Chipset Family SMBus Controller" },
    { 0x8086, 0xA2A4, "200 Series/Z370 Chipset Family SPI Controller" },
    { 0x8086, 0xA2A5, "200 Series/Z370 Chipset Family Gigabit Ethernet Controller" },
    { 0x8086, 0xA2A6, "200 Series/Z370 Chipset Family Trace Hub" },
    { 0x8086, 0xA2A7, "200 Series/Z370 Chipset Family Serial IO UART Controller #0" },
    { 0x8086, 0xA2A8, "200 Series/Z370 Chipset Family Serial IO UART Controller #1" },
    { 0x8086, 0xA2A9, "200 Series/Z370 Chipset Family Serial IO SPI Controller #0" },
    { 0x8086, 0xA2AA, "200 Series/Z370 Chipset Family Serial IO SPI Controller #1" },
    { 0x8086, 0xA2AF, "200 Series/Z370 Chipset Family USB 3.0 xHCI Controller" },
    { 0x8086, 0xA2B1, "200 Series PCH Thermal Subsystem" },
    { 0x8086, 0xA2BA, "200 Series PCH CSME HECI #1" },
    { 0x8086, 0xA2BB, "200 Series PCH CSME HECI #2" },
    { 0x8086, 0xA2C4, "200 Series PCH LPC Controller (H270)" },
    { 0x8086, 0xA2C5, "200 Series PCH LPC Controller (Z270)" },
    { 0x8086, 0xA2C6, "200 Series PCH LPC Controller (Q270)" },
    { 0x8086, 0xA2C7, "200 Series PCH LPC Controller (Q250)" },
    { 0x8086, 0xA2C8, "200 Series PCH LPC Controller (B250)" },
    { 0x8086, 0xA2C9, "Z370 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA2D2, "X299 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA2D3, "C422 Chipset LPC/eSPI Controller" },
    { 0x8086, 0xA2E0, "200 Series PCH Serial IO I2C Controller #0" },
    { 0x8086, 0xA2E1, "200 Series PCH Serial IO I2C Controller #1" },
    { 0x8086, 0xA2E2, "200 Series PCH Serial IO I2C Controller #2" },
    { 0x8086, 0xA2E3, "200 Series PCH Serial IO I2C Controller #3" },
    { 0x8086, 0xA2E6, "200 Series PCH Serial IO UART Controller #2" },
    { 0x8086, 0xA2E7, "200 Series PCH PCI Express Root Port #17" },
    { 0x8086, 0xA2E8, "200 Series PCH PCI Express Root Port #18" },
    { 0x8086, 0xA2E9, "200 Series PCH PCI Express Root Port #19" },
    { 0x8086, 0xA2EA, "200 Series PCH PCI Express Root Port #20" },
    { 0x8086, 0xA2EB, "200 Series PCH PCI Express Root Port #21" },
    { 0x8086, 0xA2EC, "200 Series PCH PCI Express Root Port #22" },
    { 0x8086, 0xA2ED, "200 Series PCH PCI Express Root Port #23" },
    { 0x8086, 0xA2EE, "200 Series PCH PCI Express Root Port #24" },
    { 0x8086, 0xA2F0, "200 Series PCH HD Audio" },
    { 0x8086, 0x1C20,
     "6 Series/C200 Series Chipset Family High Definition Audio Controller"
     },
    { 0x8086, 0x2922,
     "82801IR/IO/IH (ICH9R/DO/DH) 6 port SATA Controller" },
    { 0x8086, 0x29C0, "82G33/G31/P35/P31 Express DRAM Controller" },
    { 0x8086, 0x7000, "82371SB PIIX3 ISA" },
    { 0x8086, 0x7010, "82371SB PIIX3 IDE" },
    { 0x8086, 0x7110, "82371AB/EB/MB PIIX4 ISA" },
    { 0x8086, 0x7111, "82371AB/EB/MB PIIX4 IDE" },
    { 0x8086, 0x7113, "82371AB/EB/MB PIIX4 ACPI" },
    { 0x8086, 0x7192,
     "440BX/ZX/DX - 82443BX/ZX/DX Host bridge (AGP disabled)" },
    /* Realtek */
    { 0x10EC, 0x8139, "RTL-8100/8101L/8139 pci Fast Ethernet Adapter" },
    { 0x10EC, 0x8168, "RTL8111/8168/8411 PCI Express Gigabit Ethernet" },
    /* QEMU */
    { 0x1234, 0x1111, "QEMU Virtual Video Controller" },
    /* VirtualBox */
    { 0x80EE, 0xBEEF, "VirtualBox Graphics Adapter" },
    { 0x80EE, 0xCAFE, "VirtualBox Guest Service" },
    /* Hyper-V */
    { 0x1414, 0x5353, "Hyper-V virtual VGA" },
    /* End */
    { 0, 0, "Unknown device" }
};

static const char unknown_device_desc[] = "Unknown device";

const char *pci_device_id_to_string(pci_device_t * device)
{
    for (uint64_t i = 0;; i++) {
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

static void pci_read_bar(uint32_t id, uint32_t index, uint32_t * address,
                         uint32_t * mask)
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

void pci_get_bar(pci_bar_t * bar, uint32_t id, uint32_t index)
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
            (void *) (((uintptr_t) addr_high << 32) | (addr_low & ~0xf));
        bar->size =
            ~(((uint64_t) mask_high << 32) | (mask_low & ~0xf)) + 1;
        bar->flags = addr_low & 0xf;
    } else if (addr_low & PCI_BAR_IO) {
        /* I/O register */
        bar->u.port = (uint16_t) (addr_low & ~0x3);
        bar->size = (uint16_t) (~(mask_low & ~0x3) + 1);
        bar->flags = addr_low & 0x3;
    } else {
        /* 32-bit mmio */
        bar->u.address = (void *) (uintptr_t) (addr_low & ~0xf);
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
    pci_device_t device = { 0 };
    device.bus = bus_id;
    device.func = 0;
    device.device = dev_id;

    uint8_t func_exist = pci_func_exist(&device);
    uint8_t is_bridge = pci_is_bridge(&device);
    uint8_t has_multi_func = pci_func_exist(&device);

    if (is_bridge) {
        klogi("PCI:\t%02x:%02x.%01x - %04x:%04x [bridge] func %s\n",
              device.bus, device.device, device.func,
              device.vendor_id, device.device_id,
              func_exist ? "existed" : "not existed");
    }

    if (func_exist) {
        if (is_bridge) {
            uint8_t sub_bus_id = pci_read_sub_bus(&device);
            if (sub_bus_id != bus_id) {
                klogi("PCI:\tRead sub bus %02x\n", sub_bus_id);
                pci_scan_bus(pci_read_sub_bus(&device));
            }
        }

        device.multifunction = has_multi_func;
        device.device_id = pci_read_device_id(&device);
        device.vendor_id = pci_read_vendor_id(&device);
        device.device_class = pci_read_class(&device);
        device.subclass = pci_read_subclass(&device);
        device.prog_if = pci_read_prog_if(&device);

        klogi("PCI:\t%02x:%02x.%01x - %04x:%04x %s\n",
              device.bus, device.device, device.func,
              device.vendor_id, device.device_id,
              pci_device_id_to_string(&device));
        vec_push_back(&pci_devices, device);

        if (device.multifunction) {
            for (uint8_t func = 1; func < MAX_FUNCTION; func++) {
                pci_device_t device2 = { 0 };
                device2.bus = bus_id;
                device2.func = func;
                device2.device = dev_id;

                if (pci_func_exist(&device2)) {
                    device2.device_id = pci_read_device_id(&device2);
                    device2.vendor_id = pci_read_vendor_id(&device2);
                    device2.device_class = pci_read_class(&device2);
                    device2.subclass = pci_read_subclass(&device2);
                    device2.prog_if = pci_read_prog_if(&device2);

                    klogi("PCI:\t%02x:%02x.%01x - %04x:%04x %s\n",
                          device2.bus, device2.device, device2.func,
                          device2.vendor_id, device2.device_id,
                          pci_device_id_to_string(&device2));
                    vec_push_back(&pci_devices, device2);
                }
            }
        }
    }
}

void pci_scan_bus(uint8_t bus_id)
{
    for (uint64_t dev = 0; dev < MAX_DEVICE; dev++) {
        pci_scan_device(bus_id, dev);
    }
}

void pci_init(void)
{
    for (uint64_t bus_id = 0; bus_id < MAX_BUS; bus_id++) {
        for (uint64_t dev = 0; dev < MAX_DEVICE; dev++) {
            pci_scan_device(bus_id, dev);
        }
    }

    klogi("PCI: Full recursive device scan done, [%ld] devices found\n",
          vec_length(&pci_devices));
}

void pci_list(void)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);
        kprintf("PCI:\t%02x:%02x.%01x - %04x:%04x %s\n",
                dev.bus, dev.device, dev.func, dev.vendor_id,
                dev.device_id, pci_device_id_to_string(&dev));
    }
}

bool pci_find(uint16_t vendor_id, uint16_t device_id, pci_device_t * out)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);

        if (dev.vendor_id == vendor_id && dev.device_id == device_id) {
            if (out != NULL)
                *out = dev;
            return true;
        }
    }
    return false;
}

bool pci_find_class(uint8_t device_class, uint8_t subclass, pci_device_t * out)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);

        if (dev.device_class == device_class && dev.subclass == subclass) {
            if (out != NULL)
                *out = dev;
            return true;
        }
    }
    return false;
}

bool pci_find_class_any(uint8_t device_class, pci_device_t * out)
{
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t dev = vec_at(&pci_devices, i);

        if (dev.device_class == device_class) {
            if (out != NULL)
                *out = dev;
            return true;
        }
    }
    return false;
}
