/**-----------------------------------------------------------------------------

 @file    vfs_srv.c
 @brief   Spawn the userspace VFS server

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>
#include <libc/bootinfo.h>
#include <libc/protocol.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/vfs_srv.h>
#include <ipc/ipc.h>
#include <mm/ipc_buf.h>
#include <service/service.h>
#include <fs/vfs.h>
#include <proc/sched.h>

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
    tc->bootinfo = bi;

    klogi("vfs: attached service endpoint to pid %ld\n", (long)tc->pid);
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

    service_register(SVC_FS, vfs_ep, tc->pid);
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

    if (service_forward(SVC_FS, &req, &rep))
        klogi("vfs: PING reply 0x%lx %s\n", rep.words[0],
              rep.words[0] == VFS_PONG ? "OK" : "BAD");
    else
        klogw("vfs: PING failed\n");

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    req.words[0] = 0;           /* F_OK */
    memcpy(&req.words[1], "/", 2);
    if (service_forward(SVC_FS, &req, &rep))
        klogi("vfs: FACCESSAT / -> %ld\n", (int64_t) rep.words[0]);

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    memcpy(&req.words[1], "/nope", 6);
    if (service_forward(SVC_FS, &req, &rep))
        klogi("vfs: FACCESSAT /nope -> %ld\n", (int64_t) rep.words[0]);

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
            if (service_forward(SVC_FS, &req, &rep))
                klogi("vfs: FACCESSAT long -> %ld\n", (int64_t) rep.words[0]);
        }
    }

    /* Open/read/close through the kernel's server-backed fd path. */
    {
        const char *path = "synthetic";
        handle_t ph;

        if (ipc_buf_from_kernel(path, strlen(path) + 1, &ph) == 0) {
            ipc_msg_t or;

            memset(&req, 0, sizeof(req));
            req.tag = VFS_OPENAT;
            req.words[0] = 2;   /* O_RDONLY */
            req.xfer[0] = ph;
            req.xfer_count = 1;

            if (service_forward(SVC_FS, &req, &or)
                && (int64_t) or.words[0] == 0) {
                vfs_handle_t fh = vfs_open_server((int64_t) or.words[1], path,
                                                  VFS_MODE_READ);
                char buf[64] = { 0 };
                int64_t n = (fh != VFS_INVALID_HANDLE)
                    ? vfs_read(fh, sizeof(buf) - 1, buf) : -1;

                if (n > 0)
                    klogi("vfs: read %ld bytes: %s", n, buf);
                else
                    klogw("vfs: read failed (%ld)\n", n);
                if (fh != VFS_INVALID_HANDLE)
                    vfs_close(fh);
            } else {
                klogw("vfs: OPENAT failed\n");
            }
        }
    }
}
