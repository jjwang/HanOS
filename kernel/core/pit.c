/**-----------------------------------------------------------------------------

 @file    pit.c
 @brief   Implementation of PIT related functions
 @details
 @verbatim

   This PIT wait function should be rewritten based on IRQ mechanism, then it
   can be used in different processes at the same time.

   The PIT wait should only be used in system initialization stage. It will not
   be re-written after re-considering it's usage.

 @endverbatim
 @author  JW
 @date    Mar 12, 2022

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <core/pit.h>
#include <core/cpu.h>
#include <core/idt.h>
#include <core/isr_base.h>
#include <lib/lock.h>
#include <lib/klog.h>

static volatile uint64_t pit_ticks = 0;

static void pit_callback()
{
    pit_ticks++;
}

uint64_t pit_get_ticks(void)
{
    return pit_ticks;
}

void pit_init(void)
{
    klogi("PIT: Set frequency to %dHz\n", PIT_FREQ_HZ);

    uint16_t x = 1193182 / PIT_FREQ_HZ;
    if ((1193182 % PIT_FREQ_HZ) > (PIT_FREQ_HZ / 2))
        x++;

    port_outb(0x40, (uint8_t)(x & 0x00ff));
    port_io_wait();
    port_outb(0x40, (uint8_t)((x & 0xff00) >> 8));
    port_io_wait();

    exc_register_handler(IRQ0, pit_callback);

    irq_clear_mask(0);
}

void pit_wait(uint64_t ms) 
{
    volatile uint64_t target_ticks = pit_ticks + ms;

    while (true) {
        if (pit_ticks >= target_ticks) break;
        asm volatile ("nop;");
    }
}

