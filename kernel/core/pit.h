/**-----------------------------------------------------------------------------

 @file    pit.h
 @brief   Definition of PIT related data structures
 @details
 @verbatim

  The Programmable Interval Timer (PIT) chip (Intel 8253/8254) basically
  consists of an oscillator, a prescaler and 3 independent frequency dividers.
  Each frequency divider has an output, which is used to allow the timer to
  control external circuitry (for example, IRQ 0).

 @endverbatim
 @author  JW
 @date    Mar 12, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#define PIT_FREQ_HZ         1000

void pit_init(void);
void pit_wait(uint64_t ms);
uint64_t pit_get_ticks(void);
