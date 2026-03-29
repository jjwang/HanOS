/**-----------------------------------------------------------------------------

 @file    spinlock.h
 @brief   Definition of spinlock related data structures and functions
 @details
 @verbatim

  Busy waiting is a technique in which a process repeatedly checks to see if a
  condition is true (from Wikipedia).

  Spinlock uses the above technique for the purpose of checking if a lock is
  available.
  
  Three functions are implemented here: init (new), acquire (lock) and release.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef volatile struct {
    uint32_t locked;            /* The value is zero if-and-only-if the lock is
                                 * in the unlocked stated */
    uint64_t rflags;
} lock_t;

#define lock_new()          (lock_t){0}
#define lock_try(x)         spin_lock_impl(x, false)
#define lock_lock(x)        spin_lock_impl(x, true)
#define lock_release(x)     spin_unlock_impl(x)

bool spin_lock_impl(lock_t *s, bool waiting);
void spin_unlock_impl(lock_t *s);

