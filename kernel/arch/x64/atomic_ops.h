#pragma once

#include <stdint.h>

/* Atomic inc 1 for uint64_t, return old value */
static inline uint64_t atomic_inc64(volatile uint64_t *ptr)
{
    uint64_t old_val = 1;
    __asm__ __volatile__ (
        "lock xadd %0, %1"
        : "+r"(old_val), "+m"(*ptr)
        : 
        : "memory", "cc"
    );
    return old_val;
}

/* Atomic add val for uint64_t, return old value */
static inline uint64_t atomic_add64(volatile uint64_t *ptr, uint64_t add)
{
    uint64_t old_val = add;
    __asm__ __volatile__ (
        "lock xadd %0, %1"
        : "+r"(old_val), "+m"(*ptr)
        : 
        : "memory", "cc"
    );
    return old_val;
}

/* Atomic dec 1 for uint64_t */
#define atomic_dec64(ptr) atomic_add64(ptr, UINT64_MAX)

