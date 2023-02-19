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

typedef struct {
    size_t numpages;
    size_t size;
} memory_metadata_t;

void* kmalloc(uint64_t size);
void kmfree(void* addr);
void* kmrealloc(void* addr, size_t newsize);

