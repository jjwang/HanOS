/**-----------------------------------------------------------------------------

 @file    klib.h
 @brief   Definition of fundamental data structures, macros and functions
 @details
 @verbatim

  Currently this file includes MIN, MAX, DIV_ROUNDUP macros.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <sys/panic.h>

#define MIN(x, y)               ((x) < (y) ? (x) : (y))
#define MAX(x, y)               ((x) > (y) ? (x) : (y))

#define ROUND_DOWN(v, n)        ((v) - ((v) % (n)))
#define ROUND_UP(v, n)          ROUND_DOWN((v) + (n) - 1, n)

#define DIV_ROUNDUP(a, b)       (((a) + ((b) - 1)) / (b))
#define ALIGNUP(x, a)           (DIV_ROUNDUP(x, a) * a)

#define ASSERT(x)               { \
                                    if(!(x)) \
                                        kpanic("%s() ASSERT failed in %s:%d\n",  \
                                               __func__, __FILE__, __LINE__); \
                                }

