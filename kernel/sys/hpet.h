/**-----------------------------------------------------------------------------

 @file    hpet.h
 @brief   Definition of HPET (High Precision Event Timer) related data
          structures
 @details
 @verbatim

  HPET, or High Precision Event Timer, is a piece of hardware designed by
  Intel and Microsoft to replace older PIT and RTC. It consists of (usually
  64-bit) main counter (which counts up), as well as from 3 to 32 32-bit or
  64-bit wide comparators. HPET is programmed using memory mapped IO, and
  the base address of HPET can be found using ACPI.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <sys/acpi.h>

typedef struct [[gnu::packed]] {
    acpi_sdt_hdr_t hdr;

    uint8_t hardware_rev_id;
    uint8_t comparator_count : 5;
    uint8_t counter_size : 1;
    uint8_t reserved : 1;
    uint8_t legacy_replace : 1;
    uint16_t pci_vendor_id;

    acpi_gas_t base_addr;

    uint8_t hpet_number;
    uint16_t minimum_tick;
    uint8_t page_protection;
} hpet_sdt_t;

typedef struct [[gnu::packed]] {
    volatile uint64_t config_and_capabilities;
    volatile uint64_t comparator_value;
    volatile uint64_t fsb_interrupt_route;
    volatile uint64_t unused;
} hpet_timer_t;

typedef struct [[gnu::packed]] {
    volatile uint64_t general_capabilities;
    volatile uint64_t unused0;
    volatile uint64_t general_configuration;
    volatile uint64_t unused1;
    volatile uint64_t general_int_status;
    volatile uint64_t unused2;
    volatile uint64_t unused3[2][12];
    volatile uint64_t main_counter_value;
    volatile uint64_t unused4;
    hpet_timer_t timers[];
} hpet_t;

void hpet_init();
uint64_t hpet_get_nanos();
uint64_t hpet_get_millis();
void hpet_nanosleep(uint64_t nanos);
