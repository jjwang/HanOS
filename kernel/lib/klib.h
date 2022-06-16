/**-----------------------------------------------------------------------------

 @file    klib.h
 @brief   Definition of fundamental data structures, macros and functions for
          the kernel
 @details
 @verbatim

  Currently this file includes MIN, MAX, DIV_ROUNDUP macros.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define MIN(x, y)                   ((x) < (y) ? (x) : (y))
#define MAX(x, y)                   ((x) > (y) ? (x) : (y))

#define DIV_ROUNDUP(a, b)           (((a) + ((b) - 1)) / (b))

