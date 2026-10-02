/**-----------------------------------------------------------------------------

 @file    vfs_srv.c
 @brief   Spawn the userspace VFS server
 @details
 @verbatim

  Creates the service endpoint, maps the initrd into the server, spawns
  /bin/vfs, registers it with the router as SVC_FS and runs the boot probe.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>
#include <bootinfo.h>
#include <protocol.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/vfs_srv.h>
#include <ipc/ipc.h>
#include <mm/ipc_buf.h>
#include <mm/mm.h>
#include <router/router.h>
#include <fs/vfs.h>
#include <fs/initrd.h>
#include <proc/sched.h>

/* Where the boot initrd is mapped in the VFS server's address space. */
#define VFS_INITRD_VADDR    0x30000000

static endpoint_t *vfs_ep = NULL;
static pid_t vfs_spawner = PID_MAX;
static bool vfs_active = false;

static void vfs_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != vfs_spawner || vfs_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(vfs_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;

    /* Hand the server the initrd read-only so it can serve the namespace. */
    void *iaddr = NULL;
    uint64_t isize = 0;

    initrd_get(&iaddr, &isize);
    if (iaddr != NULL && isize != 0) {
        vmm_map(tc->addrspace, VFS_INITRD_VADDR, VIRT_TO_PHYS((uint64_t) iaddr),
                NUM_PAGES(isize), VMM_FLAG_PRESENT | VMM_FLAG_USER);
        bi->initrd_vaddr = VFS_INITRD_VADDR;
        bi->initrd_size = isize;
        klogi("vfs: mapped initrd 0x%lx (%ld bytes) for pid %ld\n",
              (unsigned long) VFS_INITRD_VADDR, (unsigned long) isize,
              (long) tc->pid);
    }

    tc->bootinfo = bi;

    klogi("vfs: attached service endpoint to pid %ld\n", (long) tc->pid);
}

bool vfs_server_start(void)
{
    vfs_ep = endpoint_create();
    if (vfs_ep == NULL)
        return false;

    const char *argv[] = { "vfs", NULL };

    vfs_spawner = sched_get_pid();
    sched_set_spawn_hook(vfs_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_VFS_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_FS, vfs_ep, tc->pid);
    vfs_active = true;
    klogi("vfs: server started and registered as SVC_FS\n");
    return true;
}

bool vfs_server_active(void)
{
    return vfs_active;
}

void vfs_server_probe(void)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_PING;

    if (router_forward(SVC_FS, &req, &rep))
        klogi("vfs: PING reply 0x%lx %s\n", rep.words[0],
              rep.words[0] == VFS_PONG ? "OK" : "BAD");
    else
        klogw("vfs: PING failed\n");

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    req.words[0] = 0;           /* F_OK */
    memcpy(&req.words[1], "/", 2);
    if (router_forward(SVC_FS, &req, &rep))
        klogi("vfs: FACCESSAT / -> %ld\n", (int64_t) rep.words[0]);

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    memcpy(&req.words[1], "/nope", 6);
    if (router_forward(SVC_FS, &req, &rep))
        klogi("vfs: FACCESSAT /nope -> %ld\n", (int64_t) rep.words[0]);

    /* A late entry in the archive (the tar lists root last): the server must
     * index the whole archive, not just its first entries. */
    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    memcpy(&req.words[1], "/root/churchill.txt", 20);
    if (router_forward(SVC_FS, &req, &rep))
        klogi("vfs: FACCESSAT /root/churchill.txt -> %ld\n",
              (int64_t) rep.words[0]);

    /* A long path travels in a memory object. */
    {
        const char *longp =
            "/some/very/long/path/that/exceeds/the/inline/limit/entirely";
        handle_t ph;

        if (ipc_buf_from_kernel(longp, strlen(longp) + 1, &ph) == 0) {
            memset(&req, 0, sizeof(req));
            req.tag = VFS_FACCESSAT;
            req.xfer[0] = ph;
            req.xfer_count = 1;
            if (router_forward(SVC_FS, &req, &rep))
                klogi("vfs: FACCESSAT long -> %ld\n", (int64_t) rep.words[0]);
        }
    }

    /* Stat through the server. The fd-based read/readdir/seek paths now live
     * in the process server and are exercised once it is up. */
    {
        vfs_stat_t st;

        if (vfs_server_stat_path("/bin", &st) == 0)
            klogi("vfs: STAT /bin mode 0x%x size %ld\n", st.st_mode,
                  (long) st.st_size);
        else
            klogw("vfs: STAT /bin failed\n");
    }
}
