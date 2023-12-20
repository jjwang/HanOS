/**-----------------------------------------------------------------------------

 @file    vfs.h
 @brief   Definition of VFS related data structures and functions
 @details
 @verbatim

  Like all Unix-like system, inode is the fundmental data structure of VFS which
  stores file index information. All children node pointers will be stored in
  inode. tnode is used to store tree information, e.g., parent node. node_desc
  data structure is used for every file operation, from fopen, fread to fclose. 

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <base/vector.h>
#include <base/time.h>

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

/* Syscall related data structures */
typedef int64_t dev_t;
typedef uint64_t ino_t;
typedef int64_t off_t;
typedef int32_t mode_t;
typedef int32_t nlink_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;

typedef int32_t pid_t;
typedef int32_t tid_t;
typedef int32_t uid_t;
typedef int32_t gid_t;

#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

typedef struct {
    ino_t d_ino;
    off_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[1024];
} dirent_t;

/* VFS data structure definitions */
typedef int64_t vfs_handle_t;

/* Forward declaration */
typedef struct vfs_inode_t vfs_inode_t;
typedef struct vfs_tnode_t vfs_tnode_t;

typedef enum {
    VFS_NODE_FILE,
    VFS_NODE_SYMLINK,
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

typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} vfs_timespec_t;

/* System calls return below stat structure. It is according to Linux
 * definition.
 */

/* File type and mode */
#define S_IFMT    0170000   /* bit mask for the file type bit field */

#define S_IFSOCK  0140000   /* socket */
#define S_IFLNK   0120000   /* symbolic link */
#define S_IFREG   0100000   /* regular file */
#define S_IFBLK   0060000   /* block device */
#define S_IFDIR   0040000   /* directory */
#define S_IFCHR   0020000   /* character device */
#define S_IFIFO   0010000   /* FIFO */

#define S_ISUID     04000   /* set-user-ID bit */
#define S_ISGID     02000   /* set-group-ID bit */
#define S_ISVTX     01000   /* sticky bit */

#define S_IRWXU     00700   /* owner has read, write, and execute
                             * permission */
#define S_IRUSR     00400   /* owner has read permission */
#define S_IWUSR     00200   /* owner has write permission */
#define S_IXUSR     00100   /* owner has execute permission */

#define S_IRWXG     00070   /* group has read, write, and execute
                             * permission */
#define S_IRGRP     00040   /* group has read permission */
#define S_IWGRP     00020   /* group has write permission */
#define S_IXGRP     00010   /* group has execute permission */

#define S_IRWXO     00007   /* others (not in group) have read, write,
                             * and execute permission */
#define S_IROTH     00004   /* others have read permission */
#define S_IWOTH     00002   /* others have write permission */
#define S_IXOTH     00001   /* others have execute permission */

typedef struct {
    dev_t     st_dev;       /* ID of device containing file */
    ino_t     st_ino;       /* Inode number */
    mode_t    st_mode;      /* File type and mode */
    nlink_t   st_nlink;     /* Number of hard links */
    uid_t     st_uid;       /* User ID of owner */
    gid_t     st_gid;       /* Group ID of owner */
    dev_t     st_rdev;      /* Device ID (if special file) */
    off_t     st_size;      /* Total size, in bytes */
    vfs_timespec_t st_atim; /* Time of last access */
    vfs_timespec_t st_mtim; /* Time of last modification */
    vfs_timespec_t st_ctim; /* Time of last status change */
    blksize_t st_blksize;   /* Block size for filesystem I/O */
    blkcnt_t  st_blocks;    /* Number of 512B blocks allocated */
} vfs_stat_t;

typedef struct {
    vfs_node_type_t type;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    size_t size;
} vfs_dirent_t;

/* Details about FS format */
typedef struct vfs_fsinfo_t {
    char name[16];  /* File system name */
    bool istemp;    /* For ramfs, it is true; for fat32 etc., it is false */
    vec_struct(void*) filelist;

    vfs_inode_t* (*mount)(vfs_inode_t *device);
    vfs_tnode_t* (*open)(vfs_inode_t *this, const char *path);
    int64_t (*mknode)(vfs_tnode_t *this);
    int64_t (*rmnode)(vfs_tnode_t *this);
    int64_t (*read)(vfs_inode_t *this, size_t offset, size_t len, void *buff);
    int64_t (*write)(vfs_inode_t *this, size_t offset, size_t len, const void *buff);
    int64_t (*sync)(vfs_inode_t *this);
    int64_t (*refresh)(vfs_inode_t *this);
    int64_t (*getdent)(vfs_inode_t *this, size_t pos, vfs_dirent_t *dirent);
    int64_t (*ioctl)(vfs_inode_t *this, int64_t request, int64_t arg);
} vfs_fsinfo_t;

struct vfs_tnode_t {
    char name[VFS_MAX_NAME_LEN];
    vfs_stat_t st;
    vfs_inode_t *inode;
    vfs_inode_t *parent;
};

struct vfs_inode_t {
    vfs_node_type_t type;           /* File type */
    char link[VFS_MAX_NAME_LEN];    /* Target file if file is symlink */
    size_t size;                    /* File size */
    uint32_t perms;                 /* File permission, modified by chmod */
    uint32_t uid;                   /* User id */
    uint32_t refcount;              /* Reference count, used by symlink */
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
    vfs_tnode_t *curr_dir_ent;
    size_t curr_dir_idx;
} vfs_node_desc_t;

int64_t vfs_get_parent_dir(const char *path, char *parent, char *currdir);

void vfs_init();
void vfs_register_fs(vfs_fsinfo_t *fs);
vfs_fsinfo_t* vfs_get_fs(char *name);
void vfs_debug();

vfs_handle_t vfs_open(char *path, vfs_openmode_t mode);
int64_t vfs_create(char *path, vfs_node_type_t type);
int64_t vfs_close(vfs_handle_t handle);
int64_t vfs_tell(vfs_handle_t handle);
int64_t vfs_seek(vfs_handle_t handle, size_t pos, int64_t whence);
int64_t vfs_read(vfs_handle_t handle, size_t len, void *buff);
int64_t vfs_write(vfs_handle_t handle, size_t len, const void *buff);
int64_t vfs_unlink(char *path);
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms);
int64_t vfs_refresh(vfs_handle_t handle);
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t *dirent);
int64_t vfs_mount(char *device, char *path, char *fsname);
int64_t vfs_ioctl(vfs_handle_t handle, int64_t request, int64_t arg);

dev_t vfs_new_dev_id(void);
ino_t vfs_new_ino_id(void);
