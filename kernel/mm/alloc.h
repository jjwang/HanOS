/**-----------------------------------------------------------------------------

 @file    alloc.h
 @brief   Memory allocation definitions
 @details
 @verbatim

  This file contains the function declarations and constants used for memory
  allocation within the HanOS kernel. It includes the initialization function
  for the allocator, and functions for allocating, reallocating, and freeing
  memory. It also defines the maximum size of memory that can be allocated.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>

#define ALLOC_MAX_SIZE     65536

void alloc_init();
void *alloc(uint64_t s);
void *realloc(void *addr, uint64_t s);
void free(void *addr);
