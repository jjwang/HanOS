/**-----------------------------------------------------------------------------

 @file    svc_monitor.h
 @brief   Restart a crashed service server
 @details
 @verbatim

   A server registers a restart callback when it starts. When its process
   dies, the router reports the service down and the monitor calls the
   callback again, so the domain comes back instead of stalling.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <router/router.h>

typedef bool (*service_restart_fn_t) (void);

/* Register the restart callback and name for a service. */
void svc_monitor_set(service_id_t id, service_restart_fn_t fn, const char *name);

/* Spawn the monitor thread. */
void svc_monitor_start(void);
