/**-----------------------------------------------------------------------------

 @file    sysfunc.h
 @brief   System call function declarations and related macros
 @details
 @verbatim

  This file contains the declarations of system call functions and related
  macros used in the HanOS operating system. It includes definitions for
  various file descriptor constants, access modes, and flags. The system call
  functions provide interfaces for common operations such as file handling,
  process management, and memory allocation.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <libc/stdio.h>
#include <libc/bootinfo.h>

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

/* Layout shared with the kernel ipc_msg_t. */
typedef struct {
    uint64_t tag;
    uint64_t words[6];
    uint64_t xfer[2];
    uint8_t xfer_count;
} sys_ipc_msg_t;

/* Microkernel IPC and capability calls. */
int64_t sys_ep_create(void);
int sys_ipc_send(int64_t handle, const sys_ipc_msg_t *msg);
int sys_ipc_recv(int64_t handle, sys_ipc_msg_t *msg);
int sys_ipc_recv_nb(int64_t handle, sys_ipc_msg_t *msg);
int sys_ipc_recv_timeout(int64_t handle, sys_ipc_msg_t *msg, int64_t timeout_ms);
int sys_ipc_call(int64_t handle, const sys_ipc_msg_t *req, sys_ipc_msg_t *rep);
int sys_ipc_reply(int64_t handle, const sys_ipc_msg_t *msg);
int sys_irq_bind(int64_t irq_handle, int64_t ep_handle);
int sys_irq_ack(int64_t irq_handle);
int sys_handle_close(int64_t handle);
int64_t sys_ioport_access(int op, int port, int width, int value);
int sys_bootinfo(bootinfo_t *bi);

void sys_libc_log(const char *message);
int sys_meminfo();
int sys_fork();
int sys_openat(int dirfd, const char *path, int flags);
int sys_getcwd(char *buffer, uint64_t size);
int sys_chdir(const char *path);
int sys_open(const char *path, int flags);
int sys_close(int fd);
int sys_read(int fd, void *buf, uint64_t count);
int sys_write(int fd, const void *buf, uint64_t count);
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
int sys_runcmd(const char *cmd);
