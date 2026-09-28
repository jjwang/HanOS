/**-----------------------------------------------------------------------------

 @file    pipe_srv.h
 @brief   Spawn the userspace pipe server
 @details
 @verbatim

  Declares the pipe server spawn/status helpers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool pipe_server_start(void);
bool pipe_server_active(void);
