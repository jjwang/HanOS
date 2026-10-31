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

#define MISC_UNAME          0xA0        /* out: buffer in xfer[1] */
#define MISC_CLOCK_GETTIME  0xA1        /* words[0]=clockid;
                                           out: words[1]=sec, words[2]=nsec */
#define MISC_GETENTROPY     0xA2        /* words[0]=length; buffer in xfer[1] */

#define MISC_UTS_LEN        65
#define MISC_MAX_ENTROPY    256

/**
 * @brief uname result, matching the Linux/musl utsname layout
 */
typedef struct {
    char sysname[MISC_UTS_LEN];
    char nodename[MISC_UTS_LEN];
    char release[MISC_UTS_LEN];
    char version[MISC_UTS_LEN];
    char machine[MISC_UTS_LEN];
    char domainname[MISC_UTS_LEN];
} misc_utsname_t;
