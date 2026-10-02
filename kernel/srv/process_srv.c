/**-----------------------------------------------------------------------------

 @file    process_srv.c
 @brief   Spawn the userspace process server
 @details
 @verbatim

  Creates the service endpoint, spawns /bin/process and registers it with the
  router as SVC_PROC. The server owns the per-process file-descriptor table.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <bootinfo.h>
#include <protocol.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/process_srv.h>
#include <ipc/ipc.h>
#include <router/router.h>
#include <proc/sched.h>

static endpoint_t *proc_ep = NULL;
static pid_t proc_spawner = PID_MAX;
static bool proc_active = false;

static void process_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != proc_spawner || proc_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(proc_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;

    /* Send handles so the server can drive the fd lifecycle (VFS_FD_FORK) on
     * the FS/pipe/tty servers itself. */
    endpoint_t *fs = router_lookup(SVC_FS);
    endpoint_t *pp = router_lookup(SVC_PIPE);
    endpoint_t *tt = router_lookup(SVC_TTY);
    endpoint_t *ft = router_lookup(SVC_FAT);

    if (fs != NULL)
        bi->fs_ep = handle_alloc(&tc->handles, endpoint_object(fs),
                                 HANDLE_RIGHT_SEND);
    if (pp != NULL)
        bi->pipe_ep = handle_alloc(&tc->handles, endpoint_object(pp),
                                   HANDLE_RIGHT_SEND);
    if (tt != NULL)
        bi->tty_ep = handle_alloc(&tc->handles, endpoint_object(tt),
                                  HANDLE_RIGHT_SEND);
    if (ft != NULL)
        bi->fat_ep = handle_alloc(&tc->handles, endpoint_object(ft),
                                  HANDLE_RIGHT_SEND);

    tc->bootinfo = bi;

    klogi("process: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool process_server_start(void)
{
    proc_ep = endpoint_create();
    if (proc_ep == NULL)
        return false;

    const char *argv[] = { "process", NULL };

    proc_spawner = sched_get_pid();
    sched_set_spawn_hook(process_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_PROC_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_PROC, proc_ep, tc->pid);
    proc_active = true;
    klogi("process: server started and registered as SVC_PROC\n");
    return true;
}

bool process_server_active(void)
{
    return proc_active;
}

void process_server_probe(void)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = PROC_PING;

    if (router_forward(SVC_PROC, &req, &rep))
        klogi("process: PING reply 0x%lx %s\n", rep.words[0],
              rep.words[0] == PROC_PONG ? "OK" : "BAD");
    else
        klogw("process: PING failed\n");
}

/* --- Kernel-side fd calls into the process server ------------------------ */

static int64_t proc_call(uint64_t tag, uint64_t w1, uint64_t w2, uint64_t w3,
                         uint64_t w4, ipc_msg_t *rep_out)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    process_t *t = sched_get_current_process();
    if (t == NULL)
        return -1;

    memset(&req, 0, sizeof(req));
    req.tag = tag;
    req.words[0] = (uint64_t) t->pid;
    req.words[1] = w1;
    req.words[2] = w2;
    req.words[3] = w3;
    req.words[4] = w4;

    if (!router_forward(SVC_PROC, &req, &rep))
        return -1;

    if (rep_out != NULL)
        *rep_out = rep;
    return (int64_t) rep.words[0];
}

int64_t process_fd_open(int32_t svc, int64_t server_fd, uint64_t size,
                        int64_t mode)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_OPEN, (uint64_t) svc, (uint64_t) server_fd,
                          size, (uint64_t) mode, &rep);

    return (r < 0) ? r : (int64_t) rep.words[1];
}

int64_t process_fd_get(int32_t fd, int32_t *kind, int32_t *svc, int64_t *server_fd,
                       uint64_t *size, uint64_t *seek_pos)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_GET, (uint64_t) fd, 0, 0, 0, &rep);

    if (r < 0)
        return r;
    if (kind != NULL)
        *kind = (int32_t) rep.words[1];
    if (svc != NULL)
        *svc = (int32_t) rep.words[2];
    if (server_fd != NULL)
        *server_fd = (int64_t) rep.words[3];
    if (size != NULL)
        *size = rep.words[4];
    if (seek_pos != NULL)
        *seek_pos = rep.words[5];
    return 0;
}

int64_t process_fd_close(int32_t fd, int32_t *kind, int32_t *svc, int64_t *server_fd)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_CLOSE, (uint64_t) fd, 0, 0, 0, &rep);

    if (r < 0)
        return r;
    if (kind != NULL)
        *kind = (int32_t) rep.words[1];
    if (svc != NULL)
        *svc = (int32_t) rep.words[2];
    if (server_fd != NULL)
        *server_fd = (int64_t) rep.words[3];
    return 0;
}

int64_t process_fd_dup(int32_t fd, int32_t newfd)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_DUP, (uint64_t) fd, (uint64_t) newfd, 0, 0,
                          &rep);

    return (r < 0) ? r : (int64_t) rep.words[1];
}

int64_t process_fd_seek(int32_t fd, uint64_t pos, int32_t whence)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_SEEK, (uint64_t) fd, pos, (uint64_t) whence,
                          0, &rep);

    return (r < 0) ? r : (int64_t) rep.words[1];
}

int64_t process_fd_fcntl(int32_t fd, int32_t cmd, int64_t arg)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_FD_FCNTL, (uint64_t) fd, (uint64_t) (int64_t) cmd,
                          (uint64_t) arg, 0, &rep);

    return (r < 0) ? r : (int64_t) rep.words[1];
}

/* Called in the parent's context after the fork, so it may block until the
 * server has cloned the child's descriptors and taken a reference on each. */
void process_fd_fork(int32_t parent, int32_t child)
{
    (void) parent;
    proc_call(PROC_FD_FORK, (uint64_t) child, 0, 0, 0, NULL);
}

void process_fd_exit(int32_t pid)
{
    /* Close the process's descriptors before it is reaped. Runs in the exiting
     * process's context (not under the run-queue lock), so it may block. */
    proc_call(PROC_FD_EXIT, (uint64_t) pid, 0, 0, 0, NULL);
}

/* Tell the server that the current process exited with status. The server
 * records it and later hands it to the parent's wait. */
void process_exit_notify(int64_t status)
{
    proc_call(PROC_EXIT, (uint64_t) status, 0, 0, 0, NULL);
}

/* Ask the server for a reapable child of the current process. Returns the child
 * pid, 0 when none is ready and nohang is set, PROC_WAIT_BLOCK when a child
 * exists but has not exited, or a negative errno. */
int64_t process_wait(int32_t target, int32_t nohang, int64_t *status)
{
    ipc_msg_t rep;
    int64_t r = proc_call(PROC_WAIT, (uint64_t) (int64_t) target,
                          (uint64_t) (nohang != 0), 0, 0, &rep);

    if (r >= 0 && status != NULL)
        *status = (int64_t) rep.words[1];
    return r;
}
