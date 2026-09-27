/**-----------------------------------------------------------------------------

 @file    fat32_srv.h
 @brief   Spawn the userspace FAT32 server

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>

bool fat32_server_start(void);
bool fat32_server_active(void);

/* Read a file through the server and log it; used to verify the path at boot. */
void fat32_server_probe(void);
