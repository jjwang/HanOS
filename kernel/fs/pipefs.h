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

vfs_inode_t* pipefs_mount(vfs_inode_t *at);
int64_t pipefs_mknode(vfs_tnode_t *this);
vfs_tnode_t* pipefs_open(vfs_inode_t *this, const char *path);
int64_t pipefs_read(vfs_inode_t *this, size_t offset, size_t len, void *buff);
int64_t pipefs_write(vfs_inode_t *this, size_t offset, size_t len, const void *buff);

void pipefs_init(void);

