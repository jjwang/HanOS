/**-----------------------------------------------------------------------------

 @file    pipe.c
 @brief   Userspace pipe server

 @details
 @verbatim

   Owns a small set of byte-stream pipes. PIPE_CREATE returns a read and a
   write end; reads and writes move data through a memory object the kernel
   moved to the server in xfer[1]. A read with no data whose write end is
   still open, or a write with no room, returns PIPE_EAGAIN so the kernel can
   retry; a read returns 0 once the write end has closed.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

#define PIPE_BUF_ADDR   0x20000000
#define PIPE_BUF_SIZE   4096
#define PIPE_MAX        16
#define PIPE_END_MAX    64

typedef struct {
    bool used;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t r_refs;            /* open read ends */
    uint32_t w_refs;            /* open write ends */
} pipe_t;

typedef struct {
    bool used;
    int pipe;
    bool is_write;
    uint32_t refs;              /* forks sharing this end */
} end_t;

static pipe_t pipes[PIPE_MAX];
static end_t ends[PIPE_END_MAX];
static bootinfo_t bi;

static int end_alloc(int pipe_idx, bool is_write)
{
    for (int i = 0; i < PIPE_END_MAX; i++) {
        if (ends[i].used)
            continue;
        ends[i].used = true;
        ends[i].pipe = pipe_idx;
        ends[i].is_write = is_write;
        ends[i].refs = 1;
        return i + 1;
    }
    return -1;
}

static int pipe_create(int *rfh, int *wfh)
{
    for (int p = 0; p < PIPE_MAX; p++) {
        if (pipes[p].used)
            continue;

        memset(&pipes[p], 0, sizeof(pipes[p]));
        pipes[p].used = true;

        int r = end_alloc(p, false);
        int w = end_alloc(p, true);
        if (r < 0 || w < 0) {
            if (r >= 0)
                ends[r - 1].used = false;
            if (w >= 0)
                ends[w - 1].used = false;
            pipes[p].used = false;
            return -1;
        }

        pipes[p].r_refs = 1;
        pipes[p].w_refs = 1;
        *rfh = r;
        *wfh = w;
        return 0;
    }
    return -1;
}

static void end_close(int fd)
{
    end_t *e = &ends[fd - 1];
    pipe_t *p = &pipes[e->pipe];

    if (e->refs > 0)
        e->refs--;
    if (e->refs > 0)
        return;

    if (e->is_write) {
        if (p->w_refs > 0)
            p->w_refs--;
    } else {
        if (p->r_refs > 0)
            p->r_refs--;
    }

    e->used = false;
    if (p->r_refs == 0 && p->w_refs == 0)
        p->used = false;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == PIPE_CREATE) {
        int rfh = 0, wfh = 0;

        if (pipe_create(&rfh, &wfh) != 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            return;
        }
        rep->words[0] = 0;
        rep->words[1] = (uint64_t) rfh;
        rep->words[2] = (uint64_t) wfh;
        return;
    }

    if (m->tag == PIPE_CLOSE) {
        int fd = (int) m->words[0];

        if (fd < 1 || fd > PIPE_END_MAX || !ends[fd - 1].used) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }
        end_close(fd);
        rep->words[0] = 0;
        return;
    }

    if (m->tag == VFS_FD_FORK) {
        int fd = (int) m->words[0];

        if (fd < 1 || fd > PIPE_END_MAX || !ends[fd - 1].used) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }
        ends[fd - 1].refs++;
        rep->words[0] = 0;
        return;
    }

    if (m->tag == PIPE_READ || m->tag == PIPE_WRITE) {
        int fd = (int) m->words[0];
        uint64_t len = m->words[1];
        bool is_write = (m->tag == PIPE_WRITE);
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        bool inline_data = (memh == 0);
        uint8_t tmp[PIPE_INLINE_MAX];
        uint8_t *buf = NULL;

        if (fd < 1 || fd > PIPE_END_MAX || !ends[fd - 1].used
            || ends[fd - 1].is_write != is_write
            || (inline_data && len > PIPE_INLINE_MAX)) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        pipe_t *p = &pipes[ends[fd - 1].pipe];

        if (inline_data) {
            buf = tmp;
            if (is_write && len > 0)
                memcpy(tmp, &m->words[2], len);
        } else {
            if (sys_mem_map(memh, PIPE_BUF_ADDR, is_write ? 1 : 3) == 0)
                buf = (uint8_t *) (uint64_t) PIPE_BUF_ADDR;
            if (buf == NULL) {
                sys_handle_close(memh);
                rep->words[0] = (uint64_t) (int64_t) -5;        /* -EIO */
                return;
            }
        }

        if (!is_write) {
            uint64_t n = (len < p->count) ? len : p->count;

            if (n == 0) {
                rep->words[0] = (p->w_refs == 0)
                    ? 0
                    : (uint64_t) (int64_t) PIPE_EAGAIN;
                rep->words[1] = 0;
            } else {
                for (uint64_t i = 0; i < n; i++) {
                    buf[i] = p->buf[p->tail];
                    p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
                }
                p->count -= (uint32_t) n;
                if (inline_data)
                    memcpy(&rep->words[2], tmp, n);
                rep->words[0] = 0;
                rep->words[1] = n;
            }
        } else {
            if (p->r_refs == 0) {
                rep->words[0] = (uint64_t) (int64_t) -32;   /* -EPIPE */
            } else {
                uint64_t space = PIPE_BUF_SIZE - p->count;
                uint64_t n = (len < space) ? len : space;

                if (n == 0) {
                    rep->words[0] = (uint64_t) (int64_t) PIPE_EAGAIN;
                    rep->words[1] = 0;
                } else {
                    for (uint64_t i = 0; i < n; i++) {
                        p->buf[p->head] = buf[i];
                        p->head = (p->head + 1) % PIPE_BUF_SIZE;
                    }
                    p->count += (uint32_t) n;
                    rep->words[0] = 0;
                    rep->words[1] = n;
                }
            }
        }

        if (!inline_data) {
            sys_mem_unmap(memh, PIPE_BUF_ADDR);
            sys_handle_close(memh);
        }
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 1000) != 0)
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
