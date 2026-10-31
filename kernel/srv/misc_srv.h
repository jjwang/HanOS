/**-----------------------------------------------------------------------------

 @file    misc_srv.h
 @brief   Spawn the userspace misc server
 @details
 @verbatim

   The misc server owns the clock, entropy and uname. Its start helper creates
   the endpoint, grants the HPET window and registers SVC_MISC.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool misc_server_start(void);
bool misc_server_active(void);
