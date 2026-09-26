/**-----------------------------------------------------------------------------

 @file    vfs_srv.h
 @brief   Spawn the userspace VFS server and register it with the router
 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool vfs_server_start(void);
bool vfs_server_active(void);

/* Round-trip VFS_PING through service_forward(); verifies the router path. */
void vfs_server_probe(void);
