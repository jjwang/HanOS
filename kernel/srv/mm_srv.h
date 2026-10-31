/**-----------------------------------------------------------------------------

 @file    mm_srv.h
 @brief   Spawn the userspace MM server
 @details
 @verbatim

   The allocator stays in the kernel. The MM server answers meminfo from a
   read-only stats primitive and is registered as SVC_MM.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool mm_server_start(void);
bool mm_server_active(void);
