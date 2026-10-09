/**-----------------------------------------------------------------------------

 @file    tty.c
 @brief   Userspace tty server

 @details
 @verbatim

   Owns /dev/tty. The kernel relays decoded keys from the input server to this
   server; reads return buffered keys (TTY_EAGAIN when none are pending) and
   writes are echoed to the console server as CONSOLE_WRITE_TAG messages and
   mirrored to the serial console through the kernel. Data up to TTY_INLINE_MAX
   travels inline in the message words, larger data in a memory object in
   xfer[1].

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

#define TTY_BUF_ADDR    0x20000000
#define TTY_KEY_MAX     256
#define TTY_CONSOLE_N   40      /* bytes per CONSOLE_WRITE_TAG message */
#define OUT_MAX         (128 * 1024)

static uint8_t keys[TTY_KEY_MAX];
static uint32_t khead;
static uint32_t ktail;
static uint32_t kcount;

/* A read with no pending key is deferred: the reply endpoint is kept here and
 * answered when a key arrives, so the reader blocks instead of polling. */
static int64_t wait_reply;
static uint64_t wait_len;
static bool msg_deferred;

static bootinfo_t bi;

/* Raw mode: a full-screen program turns echo and canonical input off. */
static bool tty_echo = true;
static bool tty_canon = true;

static void key_push(uint8_t k)
{
    if (kcount >= TTY_KEY_MAX)
        return;
    keys[khead] = k;
    khead = (khead + 1) % TTY_KEY_MAX;
    kcount++;
}

static uint8_t out_buf[OUT_MAX];
static uint32_t out_head;
static uint32_t out_tail;
static uint32_t out_count;

/* Send the buffered console bytes. A non-blocking pass stops when the console
 * queue is full; a blocking pass waits for it to drain. Bytes leave the buffer
 * only after the send succeeds, so nothing is dropped. */
static void out_flush(bool blocking)
{
    while (out_count > 0) {
        sys_ipc_msg_t wm;
        uint64_t k = (out_count < TTY_CONSOLE_N) ? out_count : TTY_CONSOLE_N;

        memset(&wm, 0, sizeof(wm));
        wm.tag = CONSOLE_WRITE_TAG;
        for (uint64_t j = 0; j < k; j++)
            wm.words[j / 8] |=
                (uint64_t) out_buf[(out_tail + (uint32_t) j) % OUT_MAX]
                << ((j % 8) * 8);
        wm.words[5] = k;

        if (sys_ipc_send((int64_t) bi.console_ep, &wm) != 0) {
            if (!blocking)
                return;
            while (sys_ipc_send((int64_t) bi.console_ep, &wm) != 0)
                ;
        }
        out_tail = (out_tail + (uint32_t) k) % OUT_MAX;
        out_count -= (uint32_t) k;
    }
}

/* Queue console output. The server keeps answering keys while a large frame
 * drains, so the input path never stalls behind the display. */
static void out_write(const uint8_t * p, uint64_t len)
{
    /* Mirror the output to the serial console; the kernel serialises it with
     * its own log so the two streams do not interleave. A full-screen program
     * (raw mode) is skipped: its control sequences would flood the log. */
    if (tty_canon)
        sys_serial_write((const char *) p, len);

    for (uint64_t i = 0; i < len; i++) {
        if (out_count >= OUT_MAX) {
            out_flush(true);
            if (out_count >= OUT_MAX)
                return;
        }
        out_buf[out_head] = p[i];
        out_head = (out_head + 1) % OUT_MAX;
        out_count++;
    }
}

/* Echo one key from the input server to the console (framebuffer and serial).
 * A line-length guard keeps backspace from erasing the shell prompt. */
static int32_t echo_len;

static void echo_key(uint8_t k)
{
    if (!tty_echo)
        return;
    if (k == '\n') {
        out_write((const uint8_t *) "\n", 1);
        echo_len = 0;
    } else if (k == '\b') {
        if (echo_len > 0) {
            /* Backspace-space-backspace erases the cell, as a terminal does. */
            out_write((const uint8_t *) "\b \b", 3);
            echo_len--;
        }
    } else if (k >= 0x20 && k < 0x7f) {
        out_write(&k, 1);
        echo_len++;
    }
}

/* Answer the deferred read with the buffered keys. */
static void flush_deferred(void)
{
    sys_ipc_msg_t rr;
    uint64_t n = (wait_len < kcount) ? wait_len : kcount;
    uint8_t tmp[TTY_INLINE_MAX];

    if (n > TTY_INLINE_MAX)
        n = TTY_INLINE_MAX;

    for (uint64_t i = 0; i < n; i++)
        tmp[i] = keys[(ktail + i) % TTY_KEY_MAX];

    memset(&rr, 0, sizeof(rr));
    rr.tag = TTY_READ;
    rr.words[0] = 0;
    rr.words[1] = n;
    memcpy(&rr.words[2], tmp, n);

    /* Consume the keys only when the reply reaches the reader, so a stale
     * reply endpoint cannot drop input. */
    int64_t reply = wait_reply;

    wait_reply = 0;
    if (sys_ipc_send(reply, &rr) == 0) {
        ktail = (uint32_t) ((ktail + n) % TTY_KEY_MAX);
        kcount -= (uint32_t) n;
    }
    sys_handle_close(reply);
}

/* A deferred read is answered once the line is complete, so a shell gets a
 * whole command in one reply. A reader that asked for few bytes is answered as
 * soon as that many arrive, which keeps single-byte readers unbuffered. */
static bool read_ready(void)
{
    if (!tty_canon)
        return kcount > 0;

    if (kcount >= wait_len)
        return true;

    for (uint32_t i = 0; i < kcount; i++)
        if (keys[(ktail + i) % TTY_KEY_MAX] == '\n')
            return true;

    return false;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == TTY_POLL) {
        rep->words[0] = 0;
        rep->words[1] = kcount;
        return;
    }

    if (m->tag == TTY_SETMODE) {
        tty_echo = m->words[0] != 0;
        tty_canon = m->words[1] != 0;
        rep->words[0] = 0;
        return;
    }

    if (m->tag == TTY_KEY) {
        uint8_t k = (uint8_t) m->words[0];
        key_push(k);
        echo_key(k);

        /* Wake a waiting reader once a full line (or the requested length) is
         * buffered. */
        if (wait_reply != 0 && read_ready())
            flush_deferred();
        return;
    }

    if (m->tag == TTY_READ) {
        uint64_t len = m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        bool inline_data = (memh == 0);
        uint64_t n = (len < kcount) ? len : kcount;
        uint8_t tmp[TTY_INLINE_MAX];
        uint8_t *dst = NULL;

        if (n == 0) {
            /* No key yet: keep the request (and its reply endpoint) and answer
             * it when a key arrives. A newer request replaces the old one. */
            if (wait_reply != 0) {
                sys_ipc_msg_t old;

                memset(&old, 0, sizeof(old));
                old.tag = TTY_READ;
                old.words[0] = (uint64_t) (int64_t) TTY_EAGAIN;
                sys_ipc_send(wait_reply, &old);
                sys_handle_close(wait_reply);
            }
            wait_reply = (int64_t) m->xfer[0];
            wait_len = len;
            msg_deferred = true;
            return;
        }

        if (inline_data) {
            dst = tmp;
        } else {
            if (sys_mem_map(memh, TTY_BUF_ADDR, 3) == 0)
                dst = (uint8_t *) (uint64_t) TTY_BUF_ADDR;
            if (dst == NULL) {
                sys_handle_close(memh);
                rep->words[0] = (uint64_t) (int64_t) -5;
                return;
            }
        }

        for (uint64_t i = 0; i < n; i++) {
            dst[i] = keys[ktail];
            ktail = (ktail + 1) % TTY_KEY_MAX;
        }
        kcount -= (uint32_t) n;

        if (inline_data) {
            memcpy(&rep->words[2], tmp, n);
        } else {
            sys_mem_unmap(memh, TTY_BUF_ADDR);
            sys_handle_close(memh);
        }

        rep->words[0] = 0;
        rep->words[1] = n;
        return;
    }

    if (m->tag == TTY_WRITE) {
        uint64_t len = m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        bool inline_data = (memh == 0);
        uint8_t tmp[TTY_INLINE_MAX];
        const uint8_t *src = NULL;

        if (len > TTY_INLINE_MAX && inline_data) {
            rep->words[0] = (uint64_t) (int64_t) -22;
            return;
        }

        if (inline_data) {
            if (len > 0)
                memcpy(tmp, &m->words[2], len);
            src = tmp;
        } else {
            if (sys_mem_map(memh, TTY_BUF_ADDR, 1) == 0)
                src = (const uint8_t *) (uint64_t) TTY_BUF_ADDR;
            if (src == NULL) {
                sys_handle_close(memh);
                rep->words[0] = (uint64_t) (int64_t) -5;
                return;
            }
        }

        /* The shell's own output resets the echo column on a new line. */
        for (uint64_t i = 0; i < len; i++)
            if (src[i] == '\n')
                echo_len = 0;

        out_write(src, len);

        if (!inline_data) {
            sys_mem_unmap(memh, TTY_BUF_ADDR);
            sys_handle_close(memh);
        }

        rep->words[0] = 0;
        rep->words[1] = len;
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int32_t main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    for (;;) {
        /* Push pending console bytes, then poll for work when some remain so
         * the loop keeps draining without blocking the key path. */
        out_flush(false);

        sys_ipc_msg_t m;
        int32_t ready = (out_count > 0)
            ? sys_ipc_recv_nb((int64_t) bi.service_ep, &m)
            : sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 500);

        if (ready != 0)
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
