/**-----------------------------------------------------------------------------

 @file    pipefs.h
 @brief   Definition of PipeFS related data structures and functions
 @details
 @verbatim

  PipeFS is a simple file system designed to handle pipe files within the HanOS
  Kernel. It provides basic file system operations such as mounting, creating,
  removing, opening, reading, and writing pipe files. This file defines the
  necessary data structures and function prototypes used by PipeFS.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <fs/vfs.h>

typedef struct {
    char name[VFS_MAX_NAME_LEN];
} pipefs_file_t;

typedef struct {
    pipefs_file_t entry;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    vfs_inode_t *parent;
} pipefs_ident_item_t;

extern vfs_fsinfo_t pipefs;

vfs_inode_t *pipefs_mount(vfs_inode_t * at);
int64_t pipefs_mknode(vfs_tnode_t * this);
int64_t pipefs_rmnode(vfs_tnode_t * this);
vfs_tnode_t *pipefs_open(vfs_inode_t * this, const char *path);
int64_t pipefs_read(vfs_inode_t * this, uint64_t offset, uint64_t len,
                    void *buff);
int64_t pipefs_write(vfs_inode_t * this, uint64_t offset, uint64_t len,
                     const void *buff);

void pipefs_init(void);
