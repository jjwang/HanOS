#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/vector.h>
#include <lib/time.h>

/* some limits */
#define VFS_MAX_PATH_LEN    4096
#define VFS_MAX_NAME_LEN    256

#define VFS_INVALID_HANDLE  -1

typedef int vfs_handle_t;

/* forward declaration */
typedef struct vfs_inode_t vfs_inode_t;
typedef struct vfs_tnode_t vfs_tnode_t;

/* stores type of node */
typedef enum {
    VFS_NODE_FILE,
    VFS_NODE_FOLDER,
    VFS_NODE_BLOCK_DEVICE,
    VFS_NODE_CHAR_DEVICE,
    VFS_NODE_MOUNTPOINT
} vfs_node_type_t;

typedef enum {
    VFS_MODE_READ,
    VFS_MODE_WRITE,
    VFS_MODE_READWRITE
} vfs_openmode_t;

typedef struct {
    vfs_node_type_t type;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
} vfs_dirent_t;

/* details about fs format */
typedef struct vfs_fsinfo_t {
    char name[16];
    bool istemp;
    vec_struct(void*) filelist;

    vfs_inode_t* (*mount)(vfs_inode_t* device);
    vfs_tnode_t* (*open)(vfs_inode_t* this, const char* path);
    int64_t (*mknode)(vfs_tnode_t* this);
    int64_t (*read)(vfs_inode_t* this, size_t offset, size_t len, void* buff);
    int64_t (*write)(vfs_inode_t* this, size_t offset, size_t len, const void* buff);
    int64_t (*sync)(vfs_inode_t* this);
    int64_t (*refresh)(vfs_inode_t* this);
    int64_t (*getdent)(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent);
    int64_t (*ioctl)(vfs_inode_t* this, int64_t req_param, void* req_data);
} vfs_fsinfo_t;

struct vfs_tnode_t {
    char name[VFS_MAX_NAME_LEN];
    vfs_inode_t* inode;
    vfs_inode_t* parent;
    vfs_tnode_t* sibling;
};

struct vfs_inode_t {
    vfs_node_type_t type;
    size_t size;
    uint32_t perms;
    uint32_t uid;
    uint32_t refcount;
    tm_t tm;
    vfs_fsinfo_t* fs;
    void* ident;
    vfs_tnode_t* mountpoint;
    vec_struct(vfs_tnode_t*) child;
};

typedef struct {
    char path[VFS_MAX_PATH_LEN];
    vfs_tnode_t* tnode;
    vfs_inode_t* inode;
    vfs_openmode_t mode;
    size_t seek_pos;
} vfs_node_desc_t;

void vfs_init();
void vfs_register_fs(vfs_fsinfo_t* fs);
vfs_fsinfo_t* vfs_get_fs(char* name);
void vfs_debug();

vfs_handle_t vfs_open(char* path, vfs_openmode_t mode);
int64_t vfs_create(char* path, vfs_node_type_t type);
int64_t vfs_close(vfs_handle_t handle);
int64_t vfs_seek(vfs_handle_t handle, size_t pos);
int64_t vfs_read(vfs_handle_t handle, size_t len, void* buff);
int64_t vfs_write(vfs_handle_t handle, size_t len, const void* buff);
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms);
int64_t vfs_refresh(vfs_handle_t handle);
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t* dirent);
int64_t vfs_mount(char* device, char* path, char* fsname);
