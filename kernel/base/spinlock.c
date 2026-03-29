/**-----------------------------------------------------------------------------

 @file    spinlock.c
 @brief   Implementation of spinlock related functions
 @details
 @verbatim

  This file implements a simple and efficient spinlock for HanOS, designed for
  x86 multi-core (SMP) systems. It uses atomic instructions to ensure thread
  safety.

 @endverbatim

 **-----------------------------------------------------------------------------
 */

#include <stdatomic.h>
#include <libc/numeric.h>

#include <base/spinlock.h>

bool spin_lock_impl(lock_t *s, bool waiting)
{
    /* Save interrupt state and disable interrupts (CLI) */
    asm volatile (
        "pushfq\n"          /* Push RFLAGS register to stack */
        "pop %0\n"          /* Pop stack value to 'flags' (preserve original state) */
        "cli\n"             /* Clear Interrupt Flag (disable maskable interrupts) */
        : "=r" (s->rflags)  /* Output operand: %0 maps to 'flags' (write-only register) */
        :                   /* No input operands */
        : "memory"          /* Tell compiler memory may be modified (prevent optimization) */
    );

    s->rflags = s->rflags & 0x200;  /* Save only IF flag */

    /* Atomic CAS spin to acquire lock (lock cmpxchg under the hood) */
    while (true) {
        if (!__sync_bool_compare_and_swap(&(s->locked), 0, 1)) {
            asm volatile("pause" : : : "memory");
            if (!waiting) return false;
        } else {
            break;
        }
    }

    asm volatile ("mfence" : : : "memory");

    return true;
}

void spin_unlock_impl(lock_t *s)
{
    /* Memory fence to prevent critical section reordering */
    asm volatile ("mfence" : : : "memory");

    /* Unlock the spinlock (reset to free state) */
    s->locked = 0;

    /* Restore original interrupt state */
    if (s->rflags & 0x200) {
        asm volatile ("sti" : : : "memory");
        s->rflags = 0;
    }
}

