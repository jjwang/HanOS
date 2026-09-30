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

#include <stdbool.h>

bool process_server_start(void);
bool process_server_active(void);

/* Round-trip PROC_PING through router_forward(); verifies the router path. */
void process_server_probe(void);

/* fd table, served by the userspace process server. process_fd_fork() is called
 * under the run-queue lock, so it only enqueues (ipc_notify) and does not wake
 * the server. */
int64_t process_fd_open(int svc, int64_t server_fd, uint64_t size,
                        int64_t mode);
int64_t process_fd_get(int fd, int *kind, int *svc, int64_t *server_fd,
                       uint64_t *size, uint64_t *seek_pos);
int64_t process_fd_close(int fd, int *kind, int *svc, int64_t *server_fd);
int64_t process_fd_dup(int fd, int newfd);
int64_t process_fd_seek(int fd, uint64_t pos, int whence);
void process_fd_fork(int parent, int child);
void process_fd_exec(int pid);
void process_fd_exit(int pid);
