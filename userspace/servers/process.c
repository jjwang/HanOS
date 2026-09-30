/**-----------------------------------------------------------------------------

 @file    process.c
 @brief   Userspace process server

 @details
 @verbatim

   Owns the per-process file-descriptor table. The kernel opens, closes,
   duplicates, forks, execs and exits through it, and resolves a fd with
   PROC_FD_GET before doing I/O. Descriptors backed by a userspace server store
   the service and the server-side fd; descriptors backed by the in-kernel
   filesystem are marked PROC_FD_KERNEL and their content stays in the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

#define PROC_PID_MAX    128
#define PROC_FD_MAX     32
#define PROC_FD_BASE    3       /* fds 0..2 are stdin/stdout/stderr */

/* Service ids sent by the kernel; must match kernel/router/router.h. */
#define SVC_FS_ID       1
#define SVC_PIPE_ID     5
#define SVC_TTY_ID      6
#define SVC_FAT_ID      7

typedef struct {
    bool used;
    int32_t kind;
    int32_t svc;
    int64_t server_fd;
    uint64_t size;
    uint64_t seek_pos;
    int64_t mode;
} proc_fd_t;

static proc_fd_t table[PROC_PID_MAX][PROC_FD_MAX];

static bootinfo_t bi;

/* Service endpoints the process server notifies on fork. */
static uint64_t fs_ep;
static uint64_t pipe_ep;
static uint64_t tty_ep;
static uint64_t fat_ep;

static uint64_t ep_for_svc(int32_t svc)
{
    if (svc == SVC_FS_ID)
        return fs_ep;
    if (svc == SVC_PIPE_ID)
        return pipe_ep;
    if (svc == SVC_TTY_ID)
        return tty_ep;
    if (svc == SVC_FAT_ID)
        return fat_ep;
    return 0;
}

static uint64_t close_tag_for_svc(int32_t svc)
{
    if (svc == SVC_FS_ID)
        return VFS_CLOSE;
    if (svc == SVC_PIPE_ID)
        return PIPE_CLOSE;
    if (svc == SVC_FAT_ID)
        return FAT_CLOSE;
    return 0;
}

static int fd_alloc(int32_t pid, proc_fd_t **out)
{
    if (pid < 0 || pid >= PROC_PID_MAX)
        return -1;

    for (int fd = PROC_FD_BASE; fd < PROC_FD_MAX; fd++) {
        if (!table[pid][fd].used) {
            *out = &table[pid][fd];
            return fd;
        }
    }

    return -1;
}

static proc_fd_t *fd_get(int32_t pid, int32_t fd)
{
    if (pid < 0 || pid >= PROC_PID_MAX || fd < 0 || fd >= PROC_FD_MAX)
        return NULL;

    return table[pid][fd].used ? &table[pid][fd] : NULL;
}

static void reply_fd(proc_fd_t * e, sys_ipc_msg_t * rep)
{
    rep->words[1] = (uint64_t) (int64_t) e->kind;
    rep->words[2] = (uint64_t) (int64_t) e->svc;
    rep->words[3] = (uint64_t) e->server_fd;
    rep->words[4] = e->size;
    rep->words[5] = e->seek_pos;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    int32_t pid = (int32_t) m->words[0];
    int32_t fd = (int32_t) m->words[1];

    switch (m->tag) {
    case PROC_PING:
        rep->words[0] = PROC_PONG;
        return;

    case PROC_FD_OPEN:{
            proc_fd_t *e = NULL;
            int nfd = fd_alloc(pid, &e);

            if (nfd < 0) {
                rep->words[0] = (uint64_t) (int64_t) -24;       /* -EMFILE */
                return;
            }

            memset(e, 0, sizeof(*e));
            e->used = true;
            e->svc = (int32_t) m->words[1];
            e->server_fd = (int64_t) m->words[2];
            e->size = m->words[3];
            e->mode = (int64_t) m->words[4];
            e->kind = PROC_FD_SERVER;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) nfd;
            return;
        }

    case PROC_FD_GET:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;        /* -EBADF */
                return;
            }
            rep->words[0] = 0;
            reply_fd(e, rep);
            return;
        }

    case PROC_FD_CLOSE:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }
            rep->words[0] = 0;
            reply_fd(e, rep);
            e->used = false;
            return;
        }

    case PROC_FD_DUP:{
            proc_fd_t *src = fd_get(pid, fd);
            int32_t newfd = (int32_t) m->words[2];
            proc_fd_t *dst = NULL;

            if (src == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }

            if (newfd < 0) {
                newfd = fd_alloc(pid, &dst);
                if (newfd < 0) {
                    rep->words[0] = (uint64_t) (int64_t) -24;
                    return;
                }
            } else if (newfd < PROC_FD_MAX) {
                dst = &table[pid][newfd];
            } else {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }

            *dst = *src;
            dst->used = true;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) newfd;
            return;
        }

    case PROC_FD_SEEK:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }
            /* The offset is only tracked here for path-keyed (FAT) files; the
             * VFS/pipe servers advance their own offset. */
            if ((int64_t) m->words[2] == 0)          /* SEEK_SET */
                e->seek_pos = m->words[1];
            else if ((int64_t) m->words[2] == 1)     /* SEEK_CUR */
                e->seek_pos += m->words[1];
            else
                e->seek_pos = e->size + m->words[1];  /* SEEK_END */
            rep->words[0] = 0;
            rep->words[1] = e->seek_pos;
            return;
        }

    case PROC_FD_FORK:{
            int32_t child = (int32_t) m->words[1];

            if (pid < 0 || pid >= PROC_PID_MAX || child < 0
                || child >= PROC_PID_MAX) {
                rep->words[0] = (uint64_t) (int64_t) -22;
                return;
            }
            memcpy(table[child], table[pid], sizeof(table[child]));

            /* The child inherits a reference to each server-side open file
             * description, so tell the owning server to add one. */
            for (int fd = 0; fd < PROC_FD_MAX; fd++) {
                proc_fd_t *e = &table[child][fd];
                uint64_t ep;

                if (!e->used || e->kind != PROC_FD_SERVER)
                    continue;

                ep = ep_for_svc(e->svc);
                if (ep == 0)
                    continue;

                sys_ipc_msg_t fm;
                memset(&fm, 0, sizeof(fm));
                fm.tag = VFS_FD_FORK;
                fm.words[0] = (uint64_t) e->server_fd;
                sys_ipc_send((int64_t) ep, &fm);
            }

            rep->words[0] = 0;
            return;
        }

    case PROC_FD_EXEC:
        /* execve keeps the process's descriptors. */
        rep->words[0] = 0;
        return;

    case PROC_FD_EXIT:
        if (pid >= 0 && pid < PROC_PID_MAX) {
            for (int i = 0; i < PROC_FD_MAX; i++) {
                proc_fd_t *e = &table[pid][i];

                if (!e->used || e->kind != PROC_FD_SERVER)
                    continue;
                uint64_t ep = ep_for_svc(e->svc);
                uint64_t tag = close_tag_for_svc(e->svc);

                if (ep != 0 && tag != 0) {
                    sys_ipc_msg_t cm;
                    memset(&cm, 0, sizeof(cm));
                    cm.tag = tag;
                    cm.words[0] = (uint64_t) e->server_fd;
                    sys_ipc_send((int64_t) ep, &cm);
                }
            }
            memset(table[pid], 0, sizeof(table[pid]));
        }
        rep->words[0] = 0;
        return;

    default:
        break;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    fs_ep = bi.fs_ep;
    pipe_ep = bi.pipe_ep;
    tty_ep = bi.tty_ep;
    fat_ep = bi.fat_ep;

    for (;;) {
        sys_ipc_msg_t m;
        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 500) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;
        handle(&m, &rep);

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
