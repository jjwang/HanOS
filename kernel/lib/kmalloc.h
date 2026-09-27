/**-----------------------------------------------------------------------------

 @file    kmalloc.h
 @brief   Definition of memory allocation related functions
 @details
 @verbatim

  e.g., malloc, free and realloc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define MEM_MAGIC_NUM       0xABEEABEE

typedef struct {
    uint64_t magic;
    uint64_t checkno;
    uint64_t numpages;
    uint64_t size;
    char filename[512];
    uint64_t lineno;
} memory_metadata_t;

extern uint64_t kmalloc_checkno;

void *kmalloc_core(uint64_t size, const char *func, uint64_t line);
void kmfree_core(void *addr, const char *func, uint64_t line);
void *kmrealloc_core(void *addr, uint64_t newsize, const char *func,
                     uint64_t line);

void *kmalloc_chunk(uint64_t size, const char *func, uint64_t line);
void kmfree_chunk(void *addr, const char *func, uint64_t line);
void *kmrealloc_chunk(void *addr, uint64_t newsize, const char *func,
                      uint64_t line);

#define kmalloc(x)          kmalloc_core(x, __func__, __LINE__)
#define kmfree(x)           kmfree_core(x, __func__, __LINE__)
#define kmrealloc(x, y)     kmrealloc_core(x, y, __func__, __LINE__)
