#pragma once

#include <fs/vfs.h>

typedef struct {
    char name[VFS_MAX_NAME_LEN];
} ttyfs_file_t;

typedef struct {
    ttyfs_file_t entry;
    tm_t tm; 
    char name[VFS_MAX_NAME_LEN];
    vfs_inode_t *parent;
} ttyfs_ident_item_t;

extern vfs_fsinfo_t ttyfs;
extern vfs_handle_t ttyfh;

vfs_inode_t* ttyfs_mount(vfs_inode_t *at);
int64_t ttyfs_mknode(vfs_tnode_t *this);
vfs_tnode_t* ttyfs_open(vfs_inode_t *this, const char *path);
int64_t ttyfs_read(vfs_inode_t *this, size_t offset, size_t len, void *buff);
int64_t ttyfs_getdent(vfs_inode_t *this, size_t pos, vfs_dirent_t *dirent);
int64_t ttyfs_write(vfs_inode_t *this, size_t offset, size_t len, const void *buff);
int64_t ttyfs_sync(vfs_inode_t *this);
int64_t ttyfs_refresh(vfs_inode_t *this);
int64_t ttyfs_ioctl(vfs_inode_t *this, int64_t request, int64_t arg);

void ttyfs_init(void);

