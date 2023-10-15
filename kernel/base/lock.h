/**-----------------------------------------------------------------------------

 @file    lock.h
 @brief   Definition of lock related data structures and functions
 @details
 @verbatim

  e.g., lock new, lock and release.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef volatile struct {
    int lock;
    uint64_t rflags;
} lock_t;

#define lock_new()          (lock_t){0, 0}
#define lock_lock(x)        lock_lock_impl(x, __FILE__, __LINE__)
#define lock_release(x)     lock_release_impl(x, __FILE__, __LINE__)

void lock_lock_impl(lock_t *s, const char *fn, const int ln);
void lock_release_impl(lock_t *s, const char *fn, const int ln);


