/**-----------------------------------------------------------------------------

 @file    spinlock.h
 @brief   Definition of the interrupt-safe spinlock
 @details
 @verbatim

   Busy waiting is a technique in which a process repeatedly checks to see if a
   condition is true (from Wikipedia). A spinlock uses this technique to protect
   a critical section on a multiprocessor system.

   The lock is interrupt-safe: spinlock_acquire() disables interrupts on the
   local core before spinning and spinlock_release() restores the interrupt
   state that was active when the lock was acquired. The saved RFLAGS value
   belongs to the current owner only and is stored inside the lock.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    volatile uint32_t locked;   /* 0: free, 1: held */
    uint64_t irq_flags;         /* RFLAGS of the core that owns the lock */
} spinlock_t;

void spinlock_init(spinlock_t *lock);
bool spinlock_acquire(spinlock_t *lock);
bool spinlock_try_acquire(spinlock_t *lock);
void spinlock_release(spinlock_t *lock);
