#pragma once

#include <fs/vfs.h>

typedef struct {
    char name[VFS_MAX_NAME_LEN];
    void *data;
    uint64_t size;
} ramfs_file_t;

typedef struct [[gnu::packed]] {
    char     name[100];
    uint64_t mode;
    uint64_t owner_id;
    uint64_t group_id;
    uint8_t  size[12];
    uint8_t  last_modified[12];
    uint64_t checksum;  
    uint8_t  type;
    uint8_t  linked_file_name[100];
    uint8_t  indicator;
    uint8_t  version[2];
    uint8_t  owner_user_name[32];
    uint8_t  owner_group_name[32];
    uint64_t dev_major_number;
    uint64_t dev_minor_number;
    uint8_t  filename_prefix[155];
} ustar_file_t;

typedef struct {
    ramfs_file_t entry;
    vfs_node_type_t type;
    tm_t tm; 
    char name[VFS_MAX_NAME_LEN];
    char path[VFS_MAX_NAME_LEN];
    vfs_inode_t *parent;
} ramfs_ident_item_t;

extern vfs_fsinfo_t ramfs;

vfs_inode_t *ramfs_mount(vfs_inode_t *at);
int64_t ramfs_mknode(vfs_tnode_t *this);
int64_t ramfs_rmnode(vfs_tnode_t *this);
vfs_tnode_t *ramfs_open(vfs_inode_t *this, const char *path);
int64_t ramfs_read(vfs_inode_t *this, size_t offset, size_t len, void *buff);
int64_t ramfs_getdent(vfs_inode_t *this, size_t pos, vfs_dirent_t *dirent);
int64_t ramfs_write(vfs_inode_t *this, size_t offset, size_t len, const void *buff);
int64_t ramfs_sync(vfs_inode_t *this);
int64_t ramfs_refresh(vfs_inode_t *this);

void ramfs_init(void *address, uint64_t size);
