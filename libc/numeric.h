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

#include <stdbool.h>

bool itoa(int num, char* str, int len, int base);
int rand(int seed, int min, int max);

