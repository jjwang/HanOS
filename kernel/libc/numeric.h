/**-----------------------------------------------------------------------------

 @file    numeric.h
 @brief   Numeric utility functions for HanOS standard library
 @details
 @verbatim

  This file contains the declarations of utility functions for numeric
  operations in the HanOS standard library. It includes the function to
  convert an integer to a string representation (itoa) and a function to
  generate a random integer within a specified range based on a seed value.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once
#include <stdint.h>

#include <stdbool.h>

bool itoa(int32_t num, char* str, int32_t len, int32_t base);
int32_t rand(int32_t seed, int32_t min, int32_t max);

