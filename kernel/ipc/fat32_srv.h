/**-----------------------------------------------------------------------------

 @file    fat32_srv.h
 @brief   Spawn the userspace FAT32 server

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool fat32_server_start(void);
bool fat32_server_active(void);

/* Kernel-side FAT32 client used by the path routing. fat32_read_path copies at
 * most `len` bytes from `off` of the file into buf. */
int64_t fat32_stat_path(const char *path, uint64_t *size, bool *is_dir);
int64_t fat32_read_path(const char *path, uint64_t off, uint64_t len,
                        void *buf);

/* Read a file through the server and log it; used to verify the path at boot. */
void fat32_server_probe(void);
