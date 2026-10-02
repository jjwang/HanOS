/**-----------------------------------------------------------------------------

 @file    vfs.h
 @brief   Types and helpers for the userspace file service

 @details
 @verbatim

   The filesystem runs in user space. The kernel keeps the shared path, stat and
   dirent types, and forwards file operations to the VFS, FAT, pipe and tty
   servers. No file data is served in ring 0.

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
#define VFS_BLOCK_SIZE      512

/* Some definitions */
#define VFS_FDCWD           -100
#define VFS_INVALID_HANDLE  -1
/* fds 0..2 are stdin/stdout/stderr; the process server allocates the rest
 * starting at 3. */
#define VFS_MIN_HANDLE      3

/* Options for file seek */
#define SEEK_CUR            1
#define SEEK_END            2
#define SEEK_SET            3

/* Syscall related data structures */
typedef uint64_t dev_t;
typedef uint64_t ino_t;
typedef int64_t off_t;
typedef int32_t mode_t;
typedef int32_t nlink_t;
typedef int64_t blksize_t;
typedef int64_t blkcnt_t;

typedef int32_t posix_pid_t;
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

/**
 * @brief Directory entry returned by readdir
 */
typedef struct {
    ino_t d_ino;
    off_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[1024];
} dirent_t;

/* VFS data structure definitions */
typedef int64_t vfs_handle_t;

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

/**
 * @brief File timestamp with second and nanosecond parts
 */
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} vfs_timespec_t;

/* File type and mode */
#define S_IFMT    0170000       /* bit mask for the file type bit field */

#define S_IFSOCK  0140000       /* socket */
#define S_IFLNK   0120000       /* symbolic link */
#define S_IFREG   0100000       /* regular file */
#define S_IFBLK   0060000       /* block device */
#define S_IFDIR   0040000       /* directory */
#define S_IFCHR   0020000       /* character device */
#define S_IFIFO   0010000       /* FIFO */

#define S_ISUID     04000       /* set-user-ID bit */
#define S_ISGID     02000       /* set-group-ID bit */
#define S_ISVTX     01000       /* sticky bit */

#define S_IRWXU     00700       /* owner has read, write, and execute
                                 * permission */
#define S_IRUSR     00400       /* owner has read permission */
#define S_IWUSR     00200       /* owner has write permission */
#define S_IXUSR     00100       /* owner has execute permission */

#define S_IRWXG     00070       /* group has read, write, and execute
                                 * permission */
#define S_IRGRP     00040       /* group has read permission */
#define S_IWGRP     00020       /* group has write permission */
#define S_IXGRP     00010       /* group has execute permission */

#define S_IRWXO     00007       /* others (not in group) have read, write,
                                 * and execute permission */
#define S_IROTH     00004       /* others have read permission */
#define S_IWOTH     00002       /* others have write permission */
#define S_IXOTH     00001       /* others have execute permission */

/**
 * @brief File status returned by the stat family of calls
 */
typedef struct {
    dev_t st_dev;               /* ID of device containing file */
    ino_t st_ino;               /* Inode number */
    mode_t st_mode;             /* File type and mode */
    nlink_t st_nlink;           /* Number of hard links */
    uid_t st_uid;               /* User ID of owner */
    gid_t st_gid;               /* Group ID of owner */
    dev_t st_rdev;              /* Device ID (if special file) */
    off_t st_size;              /* Total size, in bytes */
    vfs_timespec_t st_atim;     /* Time of last access */
    vfs_timespec_t st_mtim;     /* Time of last modification */
    vfs_timespec_t st_ctim;     /* Time of last status change */
    blksize_t st_blksize;       /* Block size for filesystem I/O */
    blkcnt_t st_blocks;         /* Number of 512B blocks allocated */
} vfs_stat_t;

/**
 * @brief VFS directory entry with name, type and basic metadata
 */
typedef struct {
    vfs_node_type_t type;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    uint64_t size;
} vfs_dirent_t;

/**
 * @brief Open file description backing a VFS handle
 *
 * The fd table lives in the process server. A handle is resolved there and the
 * result is copied into a per-CPU scratch descriptor for the current call.
 */
typedef struct {
    char path[VFS_MAX_PATH_LEN];
    bool server;
    int64_t server_fd;
    uint64_t server_size;       /* file size reported by the server on open */
    int svc;                    /* service that owns server_fd */
    uint64_t seek_pos;
    uint64_t curr_dir_idx;
} vfs_node_desc_t;

int64_t vfs_get_parent_dir(const char *path, char *parent, char *currdir);

/* Resolve a handle to its server-side descriptor via the process server. The
 * result is a per-CPU scratch descriptor valid until the next call. */
vfs_node_desc_t *vfs_handle_to_fd(vfs_handle_t handle, const char *func);

/* Path operations. The VFS server resolves cwd+path and redirects paths under
 * the FAT mount to the FAT server. */
int64_t vfs_stat_path(const char *cwd, const char *path, vfs_stat_t * out);
int64_t vfs_access_path(const char *cwd, const char *path, uint64_t mode);
int64_t vfs_unlink_path(const char *cwd, const char *path);
vfs_handle_t vfs_open_path(const char *cwd, const char *path, int flags,
                           int *svc);

/* Register a descriptor owned by the userspace VFS server. */
vfs_handle_t vfs_open_server(int64_t server_fd, const char *path,
                             vfs_openmode_t mode, uint64_t size);
/* Same, but for any service (e.g. the pipe server). */
vfs_handle_t vfs_open_server_svc(int64_t server_fd, const char *path,
                                 vfs_openmode_t mode, uint64_t size, int svc);

/* Read a whole file into a freshly kmalloc_chunk()'d kernel buffer without
 * touching the process fd table. Used by the ELF loader, which falls back to
 * the initrd image while the userspace servers are still coming up. Returns 0
 * on success. */
int64_t vfs_load_file(const char *path, uint8_t **out_buf, uint64_t *out_len);

/* Stat an open server fd. `out` is a kernel vfs_stat_t buffer. */
int64_t vfs_server_fstat(int64_t sfd, void *out);
int64_t vfs_server_readdir(vfs_handle_t handle, void *out);
/* Take a reference on a server fd, so a description inherited across fork or
 * execve stays open. */
void vfs_server_ref_fd(int svc, int64_t sfd);
int64_t vfs_close(vfs_handle_t handle);
uint64_t vfs_tell(vfs_handle_t handle);
int64_t vfs_seek(vfs_handle_t handle, uint64_t pos, int64_t whence);
int64_t vfs_read(vfs_handle_t handle, uint64_t len, void *buff);
int64_t vfs_write(vfs_handle_t handle, uint64_t len, const void *buff);
