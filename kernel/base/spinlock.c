/**-----------------------------------------------------------------------------

 @file    spinlock.c
 @brief   Implementation of the interrupt-safe spinlock
 @details
 @verbatim

   This file implements a simple and efficient spinlock for HanOS, designed for
   x86 multi-core (SMP) systems. It uses atomic instructions to ensure thread
   safety.

   The lock stores the RFLAGS value of the core that currently owns it. The
   value is read before the lock is released, so a core that acquires the lock
   immediately afterwards can never corrupt the interrupt state of the core
   that is releasing it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */

#include <base/spinlock.h>

#define RFLAGS_IF           0x200       /* Interrupt enable flag */

static inline uint64_t spinlock_irq_save(void)
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

    return flags & RFLAGS_IF;
}

static inline void spinlock_irq_restore(uint64_t flags)
{
    if (flags & RFLAGS_IF)
        asm volatile ("sti" ::: "memory");
}

void spinlock_init(spinlock_t *lock)
{
    lock->locked = 0;
    lock->irq_flags = 0;
}

bool spinlock_acquire(spinlock_t *lock)
{
    uint64_t flags = spinlock_irq_save();

    while (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQ_REL)) {
        while (__atomic_load_n(&lock->locked, __ATOMIC_RELAXED))
            asm volatile ("pause" ::: "memory");
    }

    lock->irq_flags = flags;
    return true;
}

bool spinlock_try_acquire(spinlock_t *lock)
{
    uint64_t flags = spinlock_irq_save();

    if (__atomic_exchange_n(&lock->locked, 1, __ATOMIC_ACQ_REL)) {
        spinlock_irq_restore(flags);
        return false;
    }

    lock->irq_flags = flags;
    return true;
}

void spinlock_release(spinlock_t *lock)
{
    uint64_t flags = lock->irq_flags;

    __atomic_store_n(&lock->locked, 0, __ATOMIC_RELEASE);
    spinlock_irq_restore(flags);
}
