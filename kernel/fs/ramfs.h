#pragma once

#include <fs/vfs.h>
#include <fs/ramfs_file.h>

typedef struct {
    ramfs_file_t entry;
    tm_t tm; 
    char name[VFS_MAX_NAME_LEN];
    vfs_inode_t* parent;
} ramfs_ident_item_t;

extern vfs_fsinfo_t ramfs;

vfs_inode_t* ramfs_mount(vfs_inode_t* at);
int64_t ramfs_mknode(vfs_tnode_t* this);
vfs_tnode_t* ramfs_open(vfs_inode_t* this, const char* path);
int64_t ramfs_read(vfs_inode_t* this, size_t offset, size_t len, void* buff);
int64_t ramfs_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent);
int64_t ramfs_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff);
int64_t ramfs_sync(vfs_inode_t* this);
int64_t ramfs_refresh(vfs_inode_t* this);
