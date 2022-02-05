///-----------------------------------------------------------------------------
///
/// @file    lock.h
/// @brief   Definition of lock related data structures and functions
/// @details
///
///   e.g., lock new, lock and release.
///
/// @author  JW
/// @date    Jan 2, 2022
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef volatile struct {
    int locked;
    uint64_t rflags;
} lock_t;

#define lock_new          (lock_t){0, 0}

#define lock_lock(s)                                            \
    {                                                           \
        asm volatile(                                           \
            "pushfq;"                                           \
            "cli;"                                              \
            "lock btsl $0, %[lock];"                            \
            "jnc 2f;"                                           \
            "1:"                                                \
            "pause;"                                            \
            "btl $0, %[lock];"                                  \
            "jc 1b;"                                            \
            "lock btsl $0, %[lock];"                            \
            "jc 1b;"                                            \
            "2:"                                                \
            "pop %[flags]"                                      \
            : [lock] "=m"((s)->locked), [flags] "=m"((s)->rflags) \
            :                                                   \
            : "memory", "cc");                                  \
    }

#define lock_release(s)                         \
    {                                           \
        asm volatile("push %[flags];"           \
                     "lock btrl $0, %[lock];"   \
                     "popfq;"                   \
                     : [lock] "=m"((s)->locked)   \
                     : [flags] "m"((s)->rflags) \
                     : "memory", "cc");         \
    }

