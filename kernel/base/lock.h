/**-----------------------------------------------------------------------------

 @file    lock.h
 @brief   Definition of spin lock related data structures and functions
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
    uint32_t lock;              /* The value is zero if-and-only-if the lock is
                                 * in the unlocked stated */
    uint64_t rflags;
    uint64_t timestamp;
} lock_t;

#define lock_new()          (lock_t){0, 0, 0}
#define lock_lock(x)        lock_lock_impl(x, __FILE__, __LINE__)
#define lock_release(x)     lock_release_impl(x, __FILE__, __LINE__)

void lock_lock_impl(lock_t * s, const char *fn, const int ln);
void lock_release_impl(lock_t * s, const char *fn, const int ln);

extern volatile uint64_t total_lock_acquire_count;
extern volatile uint64_t total_lock_hold_time_ns;

