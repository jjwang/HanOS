/**-----------------------------------------------------------------------------

 @file    vfs_srv.h
 @brief   Spawn the userspace VFS server and register it with the router
 @details
 @verbatim

  Declares the VFS server spawn/status helpers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool vfs_server_start(void);
bool vfs_server_active(void);

/* Round-trip VFS_PING through router_forward(); verifies the router path. */
void vfs_server_probe(void);
