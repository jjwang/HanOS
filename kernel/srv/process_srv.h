/**-----------------------------------------------------------------------------

 @file    process_srv.h
 @brief   Spawn the userspace process server and register it with the router
 @details
 @verbatim

  Declares the process server spawn/status helpers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once
#include <stdint.h>

#include <stdbool.h>

bool process_server_start(void);
bool process_server_active(void);

/* Round-trip PROC_PING through router_forward(); verifies the router path. */
void process_server_probe(void);

/* fd table, served by the userspace process server. process_fd_fork() is called
 * under the run-queue lock, so it only enqueues (ipc_notify) and does not wake
 * the server. */
int64_t process_fd_open(int32_t svc, int64_t server_fd, uint64_t size,
                        int64_t mode);
int64_t process_fd_get(int32_t fd, int32_t *kind, int32_t *svc, int64_t *server_fd,
                       uint64_t *size, uint64_t *seek_pos);
int64_t process_fd_close(int32_t fd, int32_t *kind, int32_t *svc, int64_t *server_fd);
int64_t process_fd_dup(int32_t fd, int32_t newfd);
int64_t process_fd_seek(int32_t fd, uint64_t pos, int32_t whence);
int64_t process_fd_fcntl(int32_t fd, int32_t cmd, int64_t arg);
void process_fd_fork(int32_t parent, int32_t child);
void process_fd_exit(int32_t pid);
/* Exit bookkeeping keyed by pid, for the idle reaper's context. */
void process_fd_exit_pid(int32_t pid);

/* wait/exit bookkeeping, served by the userspace process server. */
void process_exit_notify(int64_t status);
/* Exit notify keyed by pid, for the idle reaper's context. */
void process_exit_notify_pid(int32_t pid, int64_t status);
int64_t process_wait(int32_t target, int32_t nohang, int64_t *status);
