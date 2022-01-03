///-----------------------------------------------------------------------------
///
/// @file    kmalloc.h
/// @brief   Definition of memory allocation related functions
/// @details
///
///   e.g., malloc, free and realloc.
///
/// @author  JW
/// @date    Jan 2, 2022
///
///-----------------------------------------------------------------------------
#pragma once

#include <stddef.h>
#include <stdint.h>

void* kmalloc(uint64_t size);
void kmfree(void* addr);
void* kmrealloc(void* addr, size_t newsize);
