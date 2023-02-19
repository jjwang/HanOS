/**-----------------------------------------------------------------------------

 @file    memutils.c
 @brief   Implementation of memory operation related functions
 @details
 @verbatim

  e.g., comparison, set value and copy.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <lib/memutils.h>

void* memcpy(void *dst, const void *src, uint64_t len)
{
    asm volatile("mov %[len], %%rcx;"
                 "mov %[src], %%rsi;"
                 "mov %[dst], %%rdi;"
                 "rep movsb;"
                 :   
                 : [len] "g"(len), [src] "g"(src), [dst] "g"(dst)
                 : "memory", "rcx", "rsi", "rdi");

    return dst;
}

void memset(void *addr, uint8_t val, uint64_t len)
{
    uint8_t *a = (uint8_t*)addr;
    for (uint64_t i = 0; i < len; i++)
        a[i] = val;
}

bool memcmp(const void *s1, const void *s2, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        uint8_t a = ((uint8_t*)s1)[i];
        uint8_t b = ((uint8_t*)s2)[i];

        if (a != b)
            return false;
    }
    return true;
}

