/**-----------------------------------------------------------------------------

 @file    block_srv.h
 @brief   Spawn and (for now) probe the userspace block server
 @details
 @verbatim

   The block server owns the ATA PIO ports and serves BLOCK_GET_INFO/READ/WRITE
   over IPC. The kernel only spawns it and grants the ports; the VFS server is
   the intended client. block_server_probe() is a boot-time round trip.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

#include <ipc/ipc.h>

bool block_server_start(void);
bool block_server_active(void);
endpoint_t *block_server_endpoint(void);

/* Round-trip BLOCK_GET_INFO; used to verify the server path at boot. */
void block_server_probe(void);
