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
    SVC_COUNT
} service_id_t;

void router_register(service_id_t id, endpoint_t *ep, pid_t owner);
endpoint_t *router_lookup(service_id_t id);
bool router_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep);
bool router_forward_timeout(service_id_t id, const ipc_msg_t *req,
                            ipc_msg_t *rep, time_t timeout_ms);
