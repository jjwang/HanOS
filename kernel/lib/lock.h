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

#define lock_new()        (lock_t){0, 0}

void lock_lock(lock_t *s);
void lock_release(lock_t *s);


