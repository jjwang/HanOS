/**-----------------------------------------------------------------------------

 @file    pipe.c
 @brief   Userspace pipe server

 @details
 @verbatim

   Owns a small set of byte-stream pipes. PIPE_CREATE returns a read and a
   write end; reads and writes move data through a memory object the kernel
   moved to the server in xfer[1]. A read with no data whose write end is still
   open, or a write with no room, is held by the server and answered once the
   other end moves data or closes; a read returns 0 once every write end has
   closed.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

#define PIPE_BUF_ADDR   0x20000000
#define PIPE_BUF_SIZE   4096
#define PIPE_MAX        16
#define PIPE_END_MAX    64
#define PIPE_WAIT_MAX   32

/**
 * @brief A byte-stream pipe buffer
 */
typedef struct {
    bool used;
    uint8_t buf[PIPE_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t r_refs;            /* open read ends */
    uint32_t w_refs;            /* open write ends */
} pipe_t;

/**
 * @brief A handle to one end of a pipe
 */
typedef struct {
    bool used;
    int pipe;
    bool is_write;
    uint32_t refs;              /* forks sharing this end */
} end_t;

static pipe_t pipes[PIPE_MAX];
static end_t ends[PIPE_END_MAX];
static bootinfo_t bi;

/* A read or write that cannot make progress is held here until data or space
 * becomes available, then answered. */
typedef struct {
    bool used;
    int pipe;
    bool is_write;
    bool inline_data;
    int64_t reply;              /* reply endpoint handle */
    int64_t memh;               /* memory object handle, 0 for inline */
    uint64_t len;
    uint8_t data[PIPE_INLINE_MAX];
} pipe_wait_t;

static pipe_wait_t waits[PIPE_WAIT_MAX];
static bool msg_deferred;

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

static void reply_waiter(pipe_wait_t * w, int64_t rc, uint64_t n,
                         const uint8_t * data)
{
    sys_ipc_msg_t rr;

    memset(&rr, 0, sizeof(rr));
    rr.tag = w->is_write ? PIPE_WRITE : PIPE_READ;
    rr.words[0] = (uint64_t) rc;
    rr.words[1] = n;
    if (rc == 0 && !w->is_write && n > 0 && w->inline_data)
        memcpy(&rr.words[2], data, n);

    sys_ipc_send(w->reply, &rr);
    sys_handle_close(w->reply);
    if (w->memh != 0)
        sys_handle_close(w->memh);
    w->used = false;
}

/* Answer a held request if the pipe can now make progress. Returns true when
 * the request was answered. */
static bool pipe_resolve(pipe_wait_t * w)
{
    pipe_t *p = &pipes[w->pipe];
    uint8_t *mapped = NULL;

    if (w->is_write) {
        if (p->r_refs == 0) {
            reply_waiter(w, -32, 0, NULL);      /* -EPIPE */
            return true;
        }

        uint64_t space = PIPE_BUF_SIZE - p->count;
        uint64_t n = (w->len < space) ? w->len : space;

        if (n == 0)
            return false;

        const uint8_t *src = w->data;

        if (!w->inline_data) {
            if (sys_mem_map(w->memh, PIPE_BUF_ADDR, 1) != 0) {
                reply_waiter(w, -5, 0, NULL);   /* -EIO */
                return true;
            }
            mapped = (uint8_t *) (uint64_t) PIPE_BUF_ADDR;
            src = mapped;
        }

        for (uint64_t i = 0; i < n; i++) {
            p->buf[p->head] = src[i];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
        }
        p->count += (uint32_t) n;

        if (mapped != NULL)
            sys_mem_unmap(w->memh, PIPE_BUF_ADDR);
        reply_waiter(w, 0, n, NULL);
        return true;
    }

    uint64_t n = (w->len < p->count) ? w->len : p->count;

    if (n == 0) {
        if (p->w_refs == 0) {
            reply_waiter(w, 0, 0, NULL);        /* EOF */
            return true;
        }
        return false;
    }

    uint8_t *dst = w->data;

    if (!w->inline_data) {
        if (sys_mem_map(w->memh, PIPE_BUF_ADDR, 3) != 0) {
            reply_waiter(w, -5, 0, NULL);
            return true;
        }
        mapped = (uint8_t *) (uint64_t) PIPE_BUF_ADDR;
        dst = mapped;
    }

    for (uint64_t i = 0; i < n; i++) {
        dst[i] = p->buf[p->tail];
        p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
    }
    p->count -= (uint32_t) n;

    if (mapped != NULL)
        sys_mem_unmap(w->memh, PIPE_BUF_ADDR);
    reply_waiter(w, 0, n, dst);
    return true;
}

/* Answer every held request on the pipe that can now make progress. A write
 * may unblock a reader, a read may unblock a writer, so repeat until steady. */
static void pipe_flush(int pi)
{
    bool progress = true;

    while (progress) {
        progress = false;

        for (int i = 0; i < PIPE_WAIT_MAX; i++) {
            if (waits[i].used && waits[i].pipe == pi && pipe_resolve(&waits[i]))
                progress = true;
        }
    }
}

/* Hold a request that cannot make progress. Returns 0, or -1 when no slot is
 * free. */
static int queue_wait(sys_ipc_msg_t * m, int pi, bool is_write,
                      bool inline_data, int64_t memh, uint64_t len)
{
    for (int i = 0; i < PIPE_WAIT_MAX; i++) {
        if (waits[i].used)
            continue;

        pipe_wait_t *w = &waits[i];

        memset(w, 0, sizeof(*w));
        w->used = true;
        w->pipe = pi;
        w->is_write = is_write;
        w->inline_data = inline_data;
        w->reply = (int64_t) m->xfer[0];
        w->memh = memh;
        w->len = len;
        if (inline_data && is_write && len > 0)
            memcpy(w->data, &m->words[2], len);
        return 0;
    }

    return -1;
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
        int pi = ends[fd - 1].pipe;

        end_close(fd);
        pipe_flush(pi);
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

        if (fd < 1 || fd > PIPE_END_MAX || !ends[fd - 1].used
            || ends[fd - 1].is_write != is_write
            || (inline_data && len > PIPE_INLINE_MAX)) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        int pi = ends[fd - 1].pipe;
        pipe_t *p = &pipes[pi];

        if (len == 0) {
            rep->words[0] = 0;
            rep->words[1] = 0;
            return;
        }

        if (!is_write) {
            uint64_t n = (len < p->count) ? len : p->count;

            if (n == 0) {
                if (p->w_refs == 0) {
                    rep->words[0] = 0;  /* EOF */
                    rep->words[1] = 0;
                    return;
                }
                if (queue_wait(m, pi, false, inline_data, memh, len) != 0) {
                    if (memh != 0)
                        sys_handle_close(memh);
                    rep->words[0] = (uint64_t) (int64_t) -12;
                    return;
                }
                msg_deferred = true;
                return;
            }

            uint8_t *mapped = NULL;
            uint8_t *dst;

            if (inline_data) {
                dst = (uint8_t *) &rep->words[2];
            } else {
                if (sys_mem_map(memh, PIPE_BUF_ADDR, 3) != 0) {
                    sys_handle_close(memh);
                    rep->words[0] = (uint64_t) (int64_t) -5;
                    return;
                }
                mapped = (uint8_t *) (uint64_t) PIPE_BUF_ADDR;
                dst = mapped;
            }

            for (uint64_t i = 0; i < n; i++) {
                dst[i] = p->buf[p->tail];
                p->tail = (p->tail + 1) % PIPE_BUF_SIZE;
            }
            p->count -= (uint32_t) n;

            if (mapped != NULL) {
                sys_mem_unmap(memh, PIPE_BUF_ADDR);
                sys_handle_close(memh);
            }

            rep->words[0] = 0;
            rep->words[1] = n;
            pipe_flush(pi);
            return;
        }

        if (p->r_refs == 0) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -32;   /* -EPIPE */
            return;
        }

        uint64_t space = PIPE_BUF_SIZE - p->count;
        uint64_t n = (len < space) ? len : space;

        if (n == 0) {
            if (queue_wait(m, pi, true, inline_data, memh, len) != 0) {
                if (memh != 0)
                    sys_handle_close(memh);
                rep->words[0] = (uint64_t) (int64_t) -12;
                return;
            }
            msg_deferred = true;
            return;
        }

        const uint8_t *src;
        uint8_t *mapped = NULL;

        if (inline_data) {
            src = (const uint8_t *) &m->words[2];
        } else {
            if (sys_mem_map(memh, PIPE_BUF_ADDR, 1) != 0) {
                sys_handle_close(memh);
                rep->words[0] = (uint64_t) (int64_t) -5;
                return;
            }
            mapped = (uint8_t *) (uint64_t) PIPE_BUF_ADDR;
            src = mapped;
        }

        for (uint64_t i = 0; i < n; i++) {
            p->buf[p->head] = src[i];
            p->head = (p->head + 1) % PIPE_BUF_SIZE;
        }
        p->count += (uint32_t) n;

        if (mapped != NULL) {
            sys_mem_unmap(memh, PIPE_BUF_ADDR);
            sys_handle_close(memh);
        }

        rep->words[0] = 0;
        rep->words[1] = n;
        pipe_flush(pi);
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
        msg_deferred = false;
        handle(&m, &rep);

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0 && !msg_deferred) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
