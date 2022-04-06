#pragma once

#include <fs/vfs.h>

extern vfs_fsinfo_t ramfs;

vfs_inode_t* ramfs_mount(vfs_inode_t* at);
int64_t ramfs_mknode(vfs_tnode_t* this);
int64_t ramfs_read(vfs_inode_t* this, size_t offset, size_t len, void* buff);
int64_t ramfs_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff);
int64_t ramfs_sync(vfs_inode_t* this);
int64_t ramfs_refresh(vfs_inode_t* this);
int64_t ramfs_setlink(vfs_tnode_t* this, vfs_inode_t* target);
