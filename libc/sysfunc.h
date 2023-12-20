#pragma once

#include <libc/stdio.h>

#define AT_FDCWD            -100

#define STDIN               0
#define STDOUT              1
#define STDERR              2

/* Reserve 3 bits for the access mode */
#define O_ACCMODE           0x0007
#define O_EXEC              1
#define O_RDONLY            2
#define O_RDWR              3
#define O_SEARCH            4
#define O_WRONLY            5

/* All remaining flags get their own bit */
#define O_APPEND            0x0008
#define O_CREAT             0x0010
#define O_DIRECTORY         0x0020
#define O_EXCL              0x0040
#define O_NOCTTY            0x0080
#define O_NOFOLLOW          0x0100
#define O_TRUNC             0x0200
#define O_NONBLOCK          0x0400
#define O_DSYNC             0x0800
#define O_RSYNC             0x1000
#define O_SYNC              0x2000
#define O_CLOEXEC           0x4000
#define O_PATH              0x8000

typedef struct {
    char command[256];
    char desc[256];
} command_help_t;

void sys_libc_log(const char *message);
int sys_meminfo();
int sys_fork();
int sys_openat(int dirfd, const char *path, int flags);
int sys_getcwd(char *buffer, size_t size);
int sys_chdir(const char *path);
int sys_open(const char *path, int flags);
int sys_close(int fd);
int sys_read(int fd, void *buf, size_t count);
int sys_write(int fd, const void *buf, size_t count);
int sys_exec(const char *path, char *const argv[]);
void sys_exit(int status);
int sys_wait(int pid);
void sys_panic(const char *message);
void *sys_malloc(int size);
int sys_mkdirat(const char *path);
int sys_dup(int fd, int flags, int newfd);
int sys_fstat(int fd, stat_t *statbuf);
int sys_stat(const char *path, stat_t *statbuf);
int sys_readdir(int fd, void *buffer);
int sys_pipe(int *fd);
int sys_unlink(const char *path);
