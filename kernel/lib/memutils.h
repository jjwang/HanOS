/**-----------------------------------------------------------------------------

 @file    memutils.h
 @brief   Definition of memory operation related functions
 @details
 @verbatim

  e.g., comparison, set value and copy.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool memcmp(const void* s1, const void* s2, uint64_t len);
void memset(void* addr, uint8_t val, uint64_t len);
void* memcpy(void* dst, const void* src, uint64_t len);

