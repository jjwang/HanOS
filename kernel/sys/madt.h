/**-----------------------------------------------------------------------------

 @file    madt.h
 @brief   Definition of ACPI MADT (Multiple APIC Description Table)
          related data structures
 @details
 @verbatim

  The MADT describes all of the interrupt controllers in the system. It
  can be used to enumerate the processors currently available.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <sys/acpi.h>

/* MADT Record Header */
typedef struct [[gnu::packed]] {
    uint8_t type;
    uint8_t len;
} madt_record_hdr_t;

/* Local APIC */
typedef struct [[gnu::packed]] {
    madt_record_hdr_t hdr;

    uint8_t proc_id;
    uint8_t apic_id;
    uint32_t flags;
} madt_record_lapic_t;

/* I/O APIC */
typedef struct [[gnu::packed]] {
    madt_record_hdr_t hdr;

    uint8_t id;
    uint8_t reserved;
    uint32_t addr;
    uint32_t gsi_base;
} madt_record_ioapic_t;

/* Interrupt Source Override */
typedef struct [[gnu::packed]] {
    madt_record_hdr_t hdr;

    uint8_t bus_src;
    uint8_t irq_src;
    uint32_t gsi;
    uint16_t flags;
} madt_record_iso_t;

/* Non Maskable Interrupt */
typedef struct [[gnu::packed]] {
    madt_record_hdr_t hdr;

    uint8_t proc_id;
    uint16_t flags;
    uint8_t lint;
} madt_record_nmi_t;

typedef struct [[gnu::packed]] {
    acpi_sdt_hdr_t hdr;

    uint32_t lapic_addr;
    uint32_t flags;

    uint8_t records[];
} madt_t;

#define MADT_RECORD_TYPE_LAPIC          0
#define MADT_RECORD_TYPE_IOAPIC         1
#define MADT_RECORD_TYPE_ISO            2
#define MADT_RECORD_TYPE_NMI            4
#define MADT_RECORD_TYPE_LAPIC_AO       5

#define MADT_LAPIC_FLAG_ENABLED         (1 << 0)
#define MADT_LAPIC_FLAG_ONLINE_CAPABLE  (1 << 1)

void madt_init();
uint32_t madt_get_num_ioapic();
uint32_t madt_get_num_lapic();
madt_record_ioapic_t** madt_get_ioapics();
madt_record_lapic_t** madt_get_lapics();
uint64_t madt_get_lapic_base();
