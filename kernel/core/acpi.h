/**-----------------------------------------------------------------------------

 @file    acpi.h
 @brief   Definition of ACPI (Advanced Configuration and Power Management
          Interface) related data structures.
 @details
 @verbatim

  ACPI (Advanced Configuration and Power Interface) is a Power Management
  and configuration standard for the PC, developed by Intel, Microsoft and
  Toshiba. ACPI allows the operating system to control the amount of power
  each device is given (allowing it to put certain devices on standby or
  power-off for example). It is also used to control and/or check thermal
  zones (temperature sensors, fan speeds, etc), battery levels, PCI IRQ
  routing, CPUs, NUMA domains and many other things.

  There are 2 main parts to ACPI. The first part is the tables used by
  the OS for configuration during boot (these include things like how many
  CPUs, APIC details, NUMA memory ranges, etc). The second part is the run
  time ACPI environment, which consists of AML code (a platform independent
  OOP language that comes from the BIOS and devices) and the ACPI SMM
  (System Management Mode) code.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <3rd-party/boot/stivale2.h>

/* RSDP (Root System Description Pointer)
 * RSDT (Root System Description Table)
 * XSDT (eXtended System Descriptor Table) - the 64-bit version of the ACPI RSDT
 *
 * To find the RSDT you need first to locate and check the RSDP, then use the
 * RsdtPointer for ACPI Version < 2.0 an XsdtPointer for any other case.
 */
typedef struct [[gnu::packed]] {
    char        sign[8];
    uint8_t     chksum;
    char        oem_id[6];
    uint8_t     revision;
    uint32_t    rsdt_addr;

    /* version 2.0 and later version */
    uint32_t    length;
    uint64_t    xsdt_addr;
    uint8_t     chksum_ext;
    uint8_t     reserved[3];
} rsdp_t;

typedef struct [[gnu::packed]] {
    char        sign[4];
    uint32_t    length;
    uint8_t     rev;
    uint8_t     chksum;
    char        oem_id[6];
    char        oem_table_id[8];
    uint32_t    oem_rev;
    uint32_t    creator_id;
    uint32_t    creator_rev;
} acpi_sdt_hdr_t;

typedef struct [[gnu::packed]] {
    acpi_sdt_hdr_t hdr;
    uint8_t data[];
} acpi_sdt_t;

typedef struct [[gnu::packed]] {
    uint8_t addr_space_id;
    uint8_t reg_bit_width;
    uint8_t reg_bit_offset;
    uint8_t reserved;
    uint64_t address;
} acpi_gas_t;

void acpi_init(struct stivale2_struct_tag_rsdp*);
acpi_sdt_t* acpi_get_sdt(const char* sign);
