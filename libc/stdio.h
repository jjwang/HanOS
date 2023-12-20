#pragma once

#include <libc/numeric.h>

#define STDIN               0
#define STDOUT              1
#define STDERR              2

#define EOF                 -1

/* ----- Definition of file system, same with vfs.h ----- */
#ifndef KERNEL_BUILD
typedef struct {
    int64_t tv_sec;
    int64_t tv_nsec;
} timespec_t;

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
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[1024];
} dirent_t;

/* File type and mode */
#define S_IFMT    0170000   /* bit mask for the file type bit field */

#define S_IFSOCK  0140000   /* socket */
#define S_IFLNK   0120000   /* symbolic link */
#define S_IFREG   0100000   /* regular file */
#define S_IFBLK   0060000   /* block device */
#define S_IFDIR   0040000   /* directory */
#define S_IFCHR   0020000   /* character device */
#define S_IFIFO   0010000   /* FIFO */

typedef struct {
    int64_t     st_dev;     /* ID of device containing file */
    uint64_t    st_ino;     /* Inode number */
    int32_t     st_mode;    /* File type and mode */
    int32_t     st_nlink;   /* Number of hard links */
    int32_t     st_uid;     /* User ID of owner */
    int32_t     st_gid;     /* Group ID of owner */
    int64_t     st_rdev;    /* Device ID (if special file) */
    int64_t     st_size;    /* Total size, in bytes */
    timespec_t  st_atim;    /* Time of last access */
    timespec_t  st_mtim;    /* Time of last modification */
    timespec_t  st_ctim;    /* Time of last status change */
    int64_t     st_blksize; /* Block size for filesystem I/O */
    int64_t     st_blocks;  /* Number of 512B blocks allocated */
} stat_t;
#else
#include <fs/vfs.h>
typedef vfs_stat_t stat_t;
#endif /* NO KERNEL_BUILD */
/* ----- Definition of file system finished ----- */

void fprintf(int fd, const char *fmt, ...);
void printf(const char *fmt, ...);

