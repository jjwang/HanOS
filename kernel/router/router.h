/**-----------------------------------------------------------------------------

 @file    router.h
 @brief   Kernel-side service router
 @details
 @verbatim

   A hybrid microkernel keeps the POSIX syscall ABI while moving services to
   user space. Each syscall domain (FS, process, ...) maps to a service id; a
   syscall wrapper calls router_lookup() and either runs the in-kernel
   implementation (NULL) or forwards an IPC request to the server endpoint.

   A service that has not registered yet returns NULL from router_lookup(),
   so the caller falls back to the in-kernel implementation.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ipc/ipc.h>
#include <proc/process.h>

typedef enum {
    SVC_MM = 0,
    SVC_FS,
    SVC_PROC,
    SVC_NET,
    SVC_MISC,
    SVC_PIPE,
    SVC_TTY,
    SVC_FAT,
    SVC_EXT,                    /* userspace ext2 filesystem */
    SVC_EVENT,                  /* kernel eventfd objects */
    SVC_EPOLL,                  /* kernel epoll objects */
    SVC_COUNT
} service_id_t;

void router_register(service_id_t id, endpoint_t *ep, pid_t owner);
endpoint_t *router_lookup(service_id_t id);
bool router_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep);
bool router_forward_timeout(service_id_t id, const ipc_msg_t *req,
                            ipc_msg_t *rep, time_t timeout_ms);
pid_t router_owner(service_id_t id);

/* Mark every service owned by pid down and drop its endpoint, so a client
 * fails fast instead of blocking on a dead server. Called when a process
 * exits. */
void router_owner_died(pid_t pid);

/* A down handler lets a supervisor learn which service died and restart it. */
void router_set_down_handler(void (*fn) (service_id_t id));
