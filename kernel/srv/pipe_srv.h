/**-----------------------------------------------------------------------------

 @file    pipe_srv.h
 @brief   Spawn the userspace pipe server

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool pipe_server_start(void);
bool pipe_server_active(void);
