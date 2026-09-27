/**-----------------------------------------------------------------------------

 @file    tty.c
 @brief   Userspace tty server

 @details
 @verbatim

   Owns /dev/tty. The kernel relays decoded keys from the input server to this
   server; reads return buffered keys (TTY_EAGAIN when none are pending) and
   writes are forwarded to the console server as CONSOLE_WRITE_TAG messages.
   Data up to TTY_INLINE_MAX travels inline in the message words, larger data
   in a memory object in xfer[1].

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
#define TTY_CONSOLE_N   5       /* bytes per CONSOLE_WRITE_TAG message */

static uint8_t keys[TTY_KEY_MAX];
static uint32_t khead;
static uint32_t ktail;
static uint32_t kcount;

static bootinfo_t bi;

static void key_push(uint8_t k)
{
    if (kcount >= TTY_KEY_MAX)
        return;
    keys[khead] = k;
    khead = (khead + 1) % TTY_KEY_MAX;
    kcount++;
}

static void console_write(const uint8_t * p, uint64_t len)
{
    uint64_t sent = 0;

    while (sent < len) {
        sys_ipc_msg_t wm;
        uint64_t k = 0;

        memset(&wm, 0, sizeof(wm));
        wm.tag = CONSOLE_WRITE_TAG;
        while (k < TTY_CONSOLE_N && sent < len)
            wm.words[k++] = p[sent++];
        wm.words[5] = k;

        for (int tries = 0; tries < 1000; tries++) {
            if (sys_ipc_send((int64_t) bi.console_ep, &wm) == 0)
                break;
        }
    }
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == TTY_KEY) {
        key_push((uint8_t) m->words[0]);
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
            rep->words[0] = (uint64_t) (int64_t) TTY_EAGAIN;
            rep->words[1] = 0;
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

        console_write(src, len);

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

int main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

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
