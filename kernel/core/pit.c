/**-----------------------------------------------------------------------------

 @file    pit.c
 @brief   Implementation of PIT related functions
 @details
 @verbatim

   This PIT sleep function should be rewritten based on IRQ mechanism, then it
   can be used in different processes at the same time.

 @endverbatim
 @author  JW
 @date    Mar 12, 2022
 @todo    Rewrite pit_wait() function

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <core/pit.h>
#include <core/cpu.h>

void pit_wait(uint64_t ms) 
{
    port_outb(0x43, 0b00110000);
    while (ms--) {
        port_outb(0x40, 0xa9);
        port_outb(0x40, 0x04);

        while (true) {
            uint8_t lo, hi; 
            lo = port_inb(0x40);
            hi = port_inb(0x40);
            // check for overflow
            if (hi > 0x04)
                break;
            hi = lo;
        }   
    }   
}

