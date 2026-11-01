/**-----------------------------------------------------------------------------

 @file    misc.h
 @brief   Protocol for the misc server
 @details
 @verbatim

   The misc server owns the clock, the entropy pool and uname. The kernel
   forwards the matching syscalls to SVC_MISC and keeps no state for them.

   A request carries the reply endpoint in xfer[0] (the router adds it) and a
   buffer memory object in xfer[1] when the result does not fit in words.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#define MISC_UNAME          0xA0        /**< uname; result buffer in xfer[1]. */
#define MISC_CLOCK_GETTIME  0xA1        /**< clock_gettime. words[0]=clockid;
                                           result: words[1]=sec, words[2]=nsec. */
#define MISC_GETENTROPY     0xA2        /**< getrandom. words[0]=length; buffer in xfer[1]. */

#define MISC_UTS_LEN        65          /**< utsname field length. */
#define MISC_MAX_ENTROPY    256         /**< Largest entropy request. */

/**
 * @brief uname result, matching the Linux/musl utsname layout
 */
typedef struct {
    char sysname[MISC_UTS_LEN];         /**< Operating system name. */
    char nodename[MISC_UTS_LEN];        /**< Network node name. */
    char release[MISC_UTS_LEN];         /**< Release version. */
    char version[MISC_UTS_LEN];         /**< Build version string. */
    char machine[MISC_UTS_LEN];         /**< Machine type. */
    char domainname[MISC_UTS_LEN];      /**< Domain name. */
} misc_utsname_t;
