/**-----------------------------------------------------------------------------

 @file    misc_srv.c
 @brief   Spawn the userspace misc server
 @details
 @verbatim

  Creates the service endpoint, grants the HPET MMIO window and the boot
  wall-clock, spawns /bin/misc and registers it with the router as SVC_MISC.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <bootinfo.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <mm/mm.h>
#include <srv/misc_srv.h>
#include <srv/svc_monitor.h>
#include <ipc/ipc.h>
#include <router/router.h>
#include <proc/sched.h>
#include <arch/x64/hpet.h>
#include <arch/x64/cmos.h>
#include <version.h>

/* Where the HPET register block is mapped in the misc server's address space. */
#define MISC_HPET_VADDR     0x30000000UL

static endpoint_t *misc_ep = NULL;
static pid_t misc_spawner = PID_MAX;
static bool misc_active = false;

static void misc_copy_string(char *dst, uint32_t size, const char *src)
{
    uint32_t i = 0;

    while (i + 1 < size && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void misc_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != misc_spawner || misc_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(misc_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;

    uint64_t phys = hpet_phys_base();

    if (phys != 0) {
        vmm_map(tc->addrspace, MISC_HPET_VADDR, phys, 1,
                VMM_FLAGS_MMIO | VMM_FLAG_USER);
        bi->misc_hpet_vaddr = MISC_HPET_VADDR;
    }

    bi->misc_hpet_period = hpet_counter_period();
    bi->misc_hpet_base = hpet_counter_base();
    bi->misc_boot_time = cmos_boot_time();
    misc_copy_string(bi->misc_version, sizeof(bi->misc_version), VERSION);

    tc->bootinfo = bi;

    klogi("misc: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool misc_server_start(void)
{
    misc_ep = endpoint_create();
    if (misc_ep == NULL)
        return false;

    const char *argv[] = { "misc", NULL };

    misc_spawner = sched_get_pid();
    sched_set_spawn_hook(misc_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_MISC_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_MISC, misc_ep, tc->pid);
    svc_monitor_set(SVC_MISC, misc_server_start, "misc");
    misc_active = true;
    klogi("misc: server started and registered as SVC_MISC\n");
    return true;
}

bool misc_server_active(void)
{
    return misc_active;
}
