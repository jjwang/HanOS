/**-----------------------------------------------------------------------------

 @file    pipe_srv.c
 @brief   Spawn the userspace pipe server
 @details
 @verbatim

  Creates the service endpoint, spawns /bin/pipe and registers it with the
  router as SVC_PIPE.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <bootinfo.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/pipe_srv.h>
#include <srv/svc_monitor.h>
#include <ipc/ipc.h>
#include <router/router.h>
#include <proc/sched.h>

static endpoint_t *pipe_ep = NULL;
static pid_t pipe_spawner = PID_MAX;
static bool pipe_active = false;

static void pipe_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != pipe_spawner || pipe_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(pipe_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;
    tc->bootinfo = bi;

    klogi("pipe: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool pipe_server_start(void)
{
    pipe_ep = endpoint_create();
    if (pipe_ep == NULL)
        return false;

    const char *argv[] = { "pipe", NULL };

    pipe_spawner = sched_get_pid();
    sched_set_spawn_hook(pipe_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_PIPE_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_PIPE, pipe_ep, tc->pid);
    svc_monitor_set(SVC_PIPE, pipe_server_start, "pipe");
    pipe_active = true;
    klogi("pipe: server started and registered as SVC_PIPE\n");
    return true;
}

bool pipe_server_active(void)
{
    return pipe_active;
}
