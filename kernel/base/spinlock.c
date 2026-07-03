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
    uint64_t flags;
    asm volatile (
        "pushfq\n"
        "pop %0\n"
        "cli\n"
        : "=r" (flags)
        :
        : "memory"
    );

    flags = flags & 0x200;

    while (true) {
        if (!__sync_bool_compare_and_swap(&(s->locked), 0, 1)) {
            asm volatile("pause" : : : "memory");
            if (!waiting) {
                if (flags & 0x200)
                    asm volatile("sti" : : : "memory");
                return false;
            }
        } else {
            break;
        }
    }

    /* Lock acquired — safe to write shared rflags now */
    s->rflags = flags;
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

