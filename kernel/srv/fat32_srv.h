/**-----------------------------------------------------------------------------

 @file    fat32_srv.h
 @brief   Spawn the userspace FAT32 server
 @details
 @verbatim

  Declares the FAT32 server spawn/status helpers and the kernel-side
  stat/read/readdir clients used by the /fat path routing.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool fat32_server_start(void);
bool fat32_server_active(void);

/* Kernel-side FAT32 client used by the path routing. The server keeps the fd
 * state (offset), so read/seek/readdir take a server fd. */
int64_t fat32_stat_path(const char *path, uint64_t *size, bool *is_dir);
int64_t fat32_open_path(const char *path, uint64_t *size);
int64_t fat32_read_fd(int64_t fd, uint64_t len, void *buf);
int64_t fat32_seek_fd(int64_t fd, uint64_t off, int64_t whence);
int64_t fat32_close_fd(int64_t fd);
int64_t fat32_fstat_fd(int64_t fd, uint64_t *size, bool *is_dir);
/* Read directory entry `index` (0-based) of an open directory: returns 0, or
 * -2 at the end. */
int64_t fat32_readdir_fd(int64_t fd, uint64_t index, char *name,
                         uint64_t namesz, uint64_t *size, bool *is_dir);

/* Read a file through the server and log it; used to verify the path at boot. */
void fat32_server_probe(void);
