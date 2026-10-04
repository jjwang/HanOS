/**-----------------------------------------------------------------------------

 @file    ioapic.h
 @brief   Definition of the I/O APIC helpers
 @details
 @verbatim

   The legacy 8259 lines are wired to the local APIC through the I/O APIC on
   most real machines. Route an ISA IRQ to a LAPIC vector and destination.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

void ioapic_init(void);
bool ioapic_available(void);

/* Program the redirection entry for an ISA IRQ line. A masked entry does not
 * deliver. */
void ioapic_set_line(uint8_t irq, bool masked);

/* Program an entry with an explicit trigger and polarity. PCI INTx lines are
 * level, active low. */
void ioapic_set_irq(uint8_t irq, bool masked, bool level, bool active_low);
