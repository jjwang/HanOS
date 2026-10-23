/**-----------------------------------------------------------------------------

 @file    block_srv.h
 @brief   Spawn and (for now) probe the userspace block server
 @details
 @verbatim

   The block server owns the AHCI controller and serves
   BLOCK_GET_INFO/READ/WRITE over IPC. The kernel only spawns it and grants the
   MMIO and DMA regions; the filesystem servers are the intended clients.
   block_server_probe() is a boot-time round trip.

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
bool block_server_probe(int32_t timeout_ms);

/* Run the probe from a kernel thread so a slow disk or USB bring-up does not
 * delay the shell. */
void block_server_probe_start(void);
/* Wait until the probe thread prints the disk summary or gives up. */
void block_server_probe_wait(void);
