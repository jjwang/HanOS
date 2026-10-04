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
#include <stdint.h>

#include <hanos.h>
#include <bootinfo.h>

#define AT_FDCWD            -100

#define STDIN               0
#define STDOUT              1
#define STDERR              2

/* Open flags (Linux values) */
#define O_ACCMODE           00000003
#define O_RDONLY            00000000
#define O_WRONLY            00000001
#define O_RDWR              00000002
#define O_CREAT             00000100
#define O_EXCL              00000200
#define O_NOCTTY            00000400
#define O_TRUNC             00001000
#define O_APPEND            00002000
#define O_NONBLOCK          00004000
#define O_DSYNC             00010000
#define O_DIRECTORY         00200000
#define O_NOFOLLOW          00400000
#define O_CLOEXEC           02000000
#define O_SYNC              04010000
#define O_PATH              010000000

/* mmap flags (Linux values) */
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20

#define PROT_NONE           0x00
#define PROT_READ           0x01
#define PROT_WRITE          0x02
#define PROT_EXEC           0x04

/* fcntl commands and descriptor flags. */
#define F_GETFD             1
#define F_SETFD             2
#define FD_CLOEXEC          1

/**
 * @brief A shell command and its help text
 */
typedef struct {
    char command[256];
    char desc[256];
} command_help_t;

/**
 * @brief An IPC message exchanged with the kernel
 *
 * The layout is shared with the kernel ipc_msg_t.
 */
typedef struct {
    uint64_t tag;
    uint64_t words[6];
    uint64_t xfer[2];
    uint8_t xfer_count;
} sys_ipc_msg_t;

/* Microkernel IPC and capability calls. */
int64_t sys_ep_create(void);
int32_t sys_ipc_send(int64_t handle, const sys_ipc_msg_t *msg);
int32_t sys_ipc_recv(int64_t handle, sys_ipc_msg_t *msg);
int32_t sys_ipc_recv_nb(int64_t handle, sys_ipc_msg_t *msg);
int32_t sys_ipc_recv_timeout(int64_t handle, sys_ipc_msg_t *msg, int64_t timeout_ms);
int32_t sys_ipc_call(int64_t handle, const sys_ipc_msg_t *req, sys_ipc_msg_t *rep);
int32_t sys_ipc_reply(int64_t handle, const sys_ipc_msg_t *msg);
int64_t sys_mem_alloc(uint64_t size);
int32_t sys_mem_map(int64_t handle, uint64_t vaddr, int32_t prot);
int32_t sys_mem_unmap(int64_t handle, uint64_t vaddr);
int32_t sys_irq_bind(int64_t irq_handle, int64_t ep_handle);
int32_t sys_irq_ack(int64_t irq_handle);
int32_t sys_handle_close(int64_t handle);
int64_t sys_handle_dup(int64_t handle);
int64_t sys_ioport_access(int32_t op, int32_t port, int32_t width, int32_t value);
int32_t sys_bootinfo(bootinfo_t *bi);

void sys_libc_log(const char *message);
int32_t sys_serial_write(const char *buf, uint64_t len);
int32_t sys_meminfo();
int32_t sys_fork();
int32_t sys_openat(int32_t dirfd, const char *path, int32_t flags);
int32_t sys_getcwd(char *buffer, uint64_t size);
int32_t sys_chdir(const char *path);
int32_t sys_resolve(const char *name, uint32_t * ip);
int32_t sys_open(const char *path, int32_t flags);
int32_t sys_close(int32_t fd);
int32_t sys_read(int32_t fd, void *buf, uint64_t count);
int32_t sys_write(int32_t fd, const void *buf, uint64_t count);
int32_t sys_exec(const char *path, char *const argv[]);
void sys_exit(int32_t status);
int32_t sys_wait(int32_t pid);
void sys_panic(const char *message);
void *sys_malloc(int32_t size);
int32_t sys_mkdirat(const char *path);
int32_t sys_dup(int32_t fd, int32_t flags, int32_t newfd);
int32_t sys_fcntl(int32_t fd, int32_t cmd, int32_t arg);
int32_t sys_socket(int32_t domain, int32_t type, int32_t protocol);
int32_t sys_bind(int32_t sock, uint32_t ip, uint16_t port);
int64_t sys_sendto(int32_t sock, uint32_t ip, uint16_t port, const void *buf,
                   uint64_t len);
int64_t sys_recvfrom(int32_t sock, void *buf, uint64_t len, uint32_t *ip,
                     uint16_t *port);
int32_t sys_socket_close(int32_t sock);
int32_t sys_connect(int32_t sock, uint32_t ip, uint16_t port);
int32_t sys_listen(int32_t sock, int32_t backlog);
int32_t sys_accept(int32_t sock);
int64_t sys_mem_phys(int64_t handle, uint64_t offset);
int32_t sys_proc_spawn(int32_t parent, const char *name);
int32_t sys_proc_map(int32_t pid, uint64_t vaddr, int32_t memh, int32_t prot);
int32_t sys_proc_set_entry(int32_t pid, uint64_t rip, uint64_t rsp);
int32_t sys_proc_start(int32_t pid);
int32_t sys_fstat(int32_t fd, stat_t *statbuf);
int32_t sys_stat(const char *path, stat_t *statbuf);
int32_t sys_readdir(int32_t fd, void *buffer);
int32_t sys_pipe(int32_t *fd);
int32_t sys_unlink(const char *path);
int32_t sys_runcmd(const char *cmd);
