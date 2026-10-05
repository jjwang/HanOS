/**-----------------------------------------------------------------------------

 @file    ext2_srv.h
 @brief   Spawn the userspace ext2 server

 @details
 @verbatim

  Declares the ext2 server spawn helper and the read-only calls the kernel
  forwards to it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool ext2_server_start(void);
bool ext2_server_active(void);

/* Resolve a mount-relative path and report its size, kind and inode mode. */
int64_t ext2_stat_path(const char *path, uint64_t * size, bool * is_dir,
                       uint32_t * mode);
int64_t ext2_open_path(const char *path, uint64_t * size, bool * is_dir,
                       uint32_t * mode);
int64_t ext2_read_fd(int64_t fd, uint64_t len, void *buf);
int64_t ext2_seek_fd(int64_t fd, uint64_t off, int64_t whence);
int64_t ext2_close_fd(int64_t fd);
int64_t ext2_fstat_fd(int64_t fd, uint64_t * size, bool * is_dir,
                      uint32_t * mode);
int64_t ext2_readdir_fd(int64_t fd, uint64_t index, char *name,
                        uint64_t namesz, uint64_t * size, bool * is_dir);

/* Read a short file through the server at boot to verify the mount. */
void ext2_server_probe(void);
