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

#define MEM_MAGIC_NUM       0xCDADDBEE

typedef struct {
    size_t magic;
    size_t checkno;
    size_t numpages;
    size_t size;
    char   filename[512];
    size_t lineno;
} memory_metadata_t;

extern size_t kmalloc_checkno;

void* kmalloc_core(uint64_t size, const char *func, size_t line);
void kmfree_core(void* addr, const char *func, size_t line);
void* kmrealloc_core(void* addr, size_t newsize, const char *func, size_t line);

#define kmalloc(x)          kmalloc_core(x, __func__, __LINE__)
#define kmfree(x)           kmfree_core(x, __func__, __LINE__)
#define kmrealloc(x, y)     kmrealloc_core(x, y, __func__, __LINE__)

