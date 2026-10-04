/**-----------------------------------------------------------------------------

 @file    svc_monitor.c
 @brief   Restart a crashed service server
 @details
 @verbatim

   The router marks a service down when its owner process exits. This
   thread polls the down flags and calls each service's restart callback,
   which creates a fresh endpoint, registers it and execs the server
   again.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <lib/klog.h>
#include <proc/sched.h>
#include <router/router.h>
#include <srv/svc_monitor.h>

typedef struct {
    service_restart_fn_t fn;
    const char *name;
} restart_t;

static restart_t restarts[SVC_COUNT];
static volatile bool pending[SVC_COUNT];

static void monitor_down(service_id_t id)
{
    if (id < SVC_COUNT)
        pending[id] = true;
}

void svc_monitor_set(service_id_t id, service_restart_fn_t fn, const char *name)
{
    if (id >= SVC_COUNT)
        return;

    restarts[id].fn = fn;
    restarts[id].name = name;
    router_set_down_handler(monitor_down);
}

_Noreturn static void svc_monitor_thread(pid_t pid)
{
    (void) pid;

    for (;;) {
        for (uint32_t id = 0; id < SVC_COUNT; id++) {
            if (!pending[id])
                continue;
            pending[id] = false;
            if (restarts[id].fn == NULL)
                continue;

            klogw("svc: restarting the %s server\n", restarts[id].name);
            if (!restarts[id].fn())
                kloge("svc: restart of %s failed\n", restarts[id].name);
        }
        sched_sleep(1000);
    }
}

void svc_monitor_start(void)
{
    process_t *t = sched_new("svcmon", svc_monitor_thread, false);

    if (t != NULL)
        sched_add(t);
}
