/**-----------------------------------------------------------------------------

 @file    service.h
 @brief   Kernel-side service router
 @details
 @verbatim

   A hybrid microkernel keeps the POSIX syscall ABI while moving services to
   user space. Each syscall domain (FS, process, ...) maps to a service id; a
   syscall wrapper calls service_lookup() and either runs the in-kernel
   implementation (NULL) or forwards an IPC request to the server endpoint.

   No service is registered yet, so the router is dormant and all syscalls
   take the in-kernel path.

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
    SVC_COUNT
} service_id_t;

void service_register(service_id_t id, endpoint_t *ep, pid_t owner);
endpoint_t *service_lookup(service_id_t id);
bool service_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep);
