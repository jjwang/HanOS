/**-----------------------------------------------------------------------------

 @file    mm_srv.c
 @brief   Spawn the userspace MM server
 @details
 @verbatim

  Creates the service endpoint, spawns /bin/mem and registers it with the
  router as SVC_MM. The allocator stays in the kernel; the server only answers
  meminfo.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <bootinfo.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/mm_srv.h>
#include <srv/svc_monitor.h>
#include <ipc/ipc.h>
#include <router/router.h>
#include <proc/sched.h>

static endpoint_t *mm_ep = NULL;
static pid_t mm_spawner = PID_MAX;
static bool mm_active = false;

static void mm_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != mm_spawner || mm_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(mm_ep),
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

    klogi("mm: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool mm_server_start(void)
{
    mm_ep = endpoint_create();
    if (mm_ep == NULL)
        return false;

    const char *argv[] = { "mem", NULL };

    mm_spawner = sched_get_pid();
    sched_set_spawn_hook(mm_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_MM_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_MM, mm_ep, tc->pid);
    svc_monitor_set(SVC_MM, mm_server_start, "mem");
    mm_active = true;
    klogi("mm: server started and registered as SVC_MM\n");
    return true;
}

bool mm_server_active(void)
{
    return mm_active;
}
