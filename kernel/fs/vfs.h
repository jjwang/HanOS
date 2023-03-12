/**-----------------------------------------------------------------------------

 @file    vfs.h
 @brief   Definition of VFS related data structures and functions
 @details
 @verbatim

  All VFS data structures and interfaces are defined in this file. 

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <lib/vector.h>
#include <lib/time.h>

/* Some limits */
#define VFS_MAX_PATH_LEN    4096
#define VFS_MAX_NAME_LEN    256

#define VFS_FDCWD           -100
#define VFS_INVALID_HANDLE  -1
#define VFS_MIN_HANDLE      100

/* Options for file seek */
#define SEEK_CUR            1
#define SEEK_END            2
#define SEEK_SET            3

typedef int vfs_handle_t;

/* Forward declaration */
typedef struct vfs_inode_t vfs_inode_t;
typedef struct vfs_tnode_t vfs_tnode_t;

/* Stores type of node */
typedef enum {
    VFS_NODE_FILE,
    VFS_NODE_FOLDER,
    VFS_NODE_BLOCK_DEVICE,
    VFS_NODE_CHAR_DEVICE,
    VFS_NODE_MOUNTPOINT,
    VFS_NODE_INVALID
} vfs_node_type_t;

typedef enum {
    VFS_MODE_READ,
    VFS_MODE_WRITE,
    VFS_MODE_READWRITE
} vfs_openmode_t;

typedef int64_t off_t;

typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef int32_t mode_t;
typedef int32_t nlink_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;

typedef int32_t pid_t;
typedef int32_t tid_t;
typedef int32_t uid_t;
typedef int32_t gid_t;

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} vfs_timespec_t;

typedef struct {
    dev_t     st_dev;
    ino_t     st_ino;
    mode_t    st_mode;
    nlink_t   st_nlink;
    uid_t     st_uid;
    gid_t     st_gid;
    dev_t     st_rdev;
    off_t     st_size;
    vfs_timespec_t st_atim;
    vfs_timespec_t st_mtim;
    vfs_timespec_t st_ctim;
    blksize_t st_blksize;
    blkcnt_t  st_blocks;
} vfs_stat_t;

typedef struct {
    vfs_node_type_t type;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    size_t size;
} vfs_dirent_t;

/* Details about fs format */
typedef struct vfs_fsinfo_t {
    char name[16];
    bool istemp;
    vec_struct(void*) filelist;

    vfs_inode_t* (*mount)(vfs_inode_t *device);
    vfs_tnode_t* (*open)(vfs_inode_t *this, const char *path);
    int64_t (*mknode)(vfs_tnode_t *this);
    int64_t (*read)(vfs_inode_t *this, size_t offset, size_t len, void *buff);
    int64_t (*write)(vfs_inode_t *this, size_t offset, size_t len, const void *buff);
    int64_t (*sync)(vfs_inode_t *this);
    int64_t (*refresh)(vfs_inode_t *this);
    int64_t (*getdent)(vfs_inode_t *this, size_t pos, vfs_dirent_t *dirent);
    int64_t (*ioctl)(vfs_inode_t *this, int64_t request, int64_t arg);
} vfs_fsinfo_t;

struct vfs_tnode_t {
    char name[VFS_MAX_NAME_LEN];
    vfs_stat_t   st;
    vfs_inode_t *inode;
    vfs_inode_t *parent;
    vfs_tnode_t *sibling;
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
    vfs_tnode_t *tnode;
    vfs_inode_t *inode;
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
int64_t vfs_tell(vfs_handle_t handle);
int64_t vfs_seek(vfs_handle_t handle, size_t pos, int64_t whence);
int64_t vfs_read(vfs_handle_t handle, size_t len, void* buff);
int64_t vfs_write(vfs_handle_t handle, size_t len, const void* buff);
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms);
int64_t vfs_refresh(vfs_handle_t handle);
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t* dirent);
int64_t vfs_mount(char* device, char* path, char* fsname);
int64_t vfs_ioctl(vfs_handle_t handle, int64_t request, int64_t arg);
