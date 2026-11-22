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

/**
 * @brief Header stored before each kmalloc block for tracking and validation
 */
typedef struct {
    uint64_t magic;
    uint64_t checkno;
    uint64_t numpages;
    uint64_t size;
    char filename[512];
    uint64_t lineno;
} memory_metadata_t;

extern uint64_t kmalloc_checkno;

void *kmalloc_impl(uint64_t size, const char *func, uint64_t line);
void kmfree_impl(void *addr, const char *func, uint64_t line);
void *kmrealloc_impl(void *addr, uint64_t newsize, const char *func,
                     uint64_t line);

void *kmalloc_pages(uint64_t size, const char *func, uint64_t line);
void kmfree_pages(void *addr, const char *func, uint64_t line);
void *kmrealloc_pages(void *addr, uint64_t newsize, const char *func,
                      uint64_t line);

#define kmalloc(x)          kmalloc_impl(x, __func__, __LINE__)
#define kmfree(x)           kmfree_impl(x, __func__, __LINE__)
#define kmrealloc(x, y)     kmrealloc_impl(x, y, __func__, __LINE__)
