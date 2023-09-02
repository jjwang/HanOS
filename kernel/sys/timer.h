/**-----------------------------------------------------------------------------

 @file    timer.h
 @brief   Definition of APIC timer related macros, data structures, functions
 @details
 @verbatim

  The great benefit of the Local APIC timer is that it is hardwired to each
  CPU core, unlike the Programmable Interval Timer which is a separate
  circuit. Because of this, there is no need for any resource management,
  which makes things easier. The downside is that it's oscillating at (one
  of) the CPU's frequencies, which varies from machine to machine, while
  the PIT uses a standard frequency (1,193,182 Hz). To make use of it, you
  have to know how many interrupts/sec it's capable of.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <base/time.h>

#define APIC_REG_TIMER_LVT          0x320
#define APIC_REG_TIMER_ICR          0x380
#define APIC_REG_TIMER_CCR          0x390
#define APIC_REG_TIMER_DCR          0x3e0

#define APIC_TIMER_FLAG_PERIODIC    (1 << 17)
#define APIC_TIMER_FLAG_MASKED      (1 << 16)

typedef enum {
    APIC_TIMER_MODE_PERIODIC,
    APIC_TIMER_MODE_ONESHOT
} apic_timer_mode_t;

void apic_timer_init(void);
void apic_timer_enable(void);
void apic_timer_stop(void);
void apic_timer_start(void);
void apic_timer_set_handler(void (*h)(void*));
void apic_timer_set_frequency(uint64_t freq);
void apic_timer_set_period(time_t tv);
void apic_timer_set_mode(apic_timer_mode_t mode);
uint8_t apic_timer_get_vector(void);

