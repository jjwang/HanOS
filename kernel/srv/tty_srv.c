/**-----------------------------------------------------------------------------

 @file    tty_srv.c
 @brief   Spawn and drive the userspace tty server

 @details
 @verbatim

   The tty server owns /dev/tty. The kernel relays decoded keys here (the input
   server produces them) and forwards stdin/stdout traffic to it; the server
   hands writes on to the console server.

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
#include <srv/tty_srv.h>
#include <ipc/ipc.h>
#include <srv/console_srv.h>
#include <router/router.h>
#include <proc/sched.h>

static endpoint_t *tty_ep = NULL;
static pid_t tty_spawner = PID_MAX;
static bool tty_active = false;

static void tty_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != tty_spawner || tty_ep == NULL)
        return;

    endpoint_t *con = console_srv_endpoint();
    if (con == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(tty_ep),
                              HANDLE_RIGHT_RECV);
    handle_t hc = handle_alloc(&tc->handles, endpoint_object(con),
                               HANDLE_RIGHT_SEND);
    if (h == HANDLE_INVALID || hc == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;
    bi->console_ep = hc;
    tc->bootinfo = bi;

    klogi("tty: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool tty_server_start(void)
{
    tty_ep = endpoint_create();
    if (tty_ep == NULL)
        return false;

    const char *argv[] = { "tty", NULL };

    tty_spawner = sched_get_pid();
    sched_set_spawn_hook(tty_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_TTY_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_TTY, tty_ep, tc->pid);
    tty_active = true;
    klogi("tty: server started and registered as SVC_TTY\n");
    return true;
}

bool tty_server_active(void)
{
    return tty_active;
}

bool tty_server_deliver_key(uint8_t key)
{
    if (!tty_active || tty_ep == NULL)
        return false;

    ipc_msg_t m;

    memset(&m, 0, sizeof(m));
    m.tag = TTY_KEY;
    m.words[0] = key;
    return ipc_send(tty_ep, &m) == 0;
}

int64_t tty_server_pending(void)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    if (!tty_server_active())
        return 0;

    memset(&req, 0, sizeof(req));
    req.tag = TTY_POLL;

    if (!router_forward(SVC_TTY, &req, &rep))
        return -1;
    if ((int64_t) rep.words[0] < 0)
        return -1;
    return (int64_t) rep.words[1];
}

int64_t tty_server_read(void *buf, uint64_t len)
{
    if (len > TTY_INLINE_MAX)
        len = TTY_INLINE_MAX;

    for (;;) {
        ipc_msg_t req;
        ipc_msg_t rep;

        memset(&req, 0, sizeof(req));
        req.tag = TTY_READ;
        req.words[0] = len;

        /* The server defers an empty read instead of answering TTY_EAGAIN, so
         * this blocks until a key arrives and the reply wakes us. */
        if (!router_forward_timeout(SVC_TTY, &req, &rep, 60 * 60 * 1000))
            return -1;

        if ((int64_t) rep.words[0] == TTY_EAGAIN) {
            sched_sleep(1);
            continue;
        }
        if ((int64_t) rep.words[0] < 0)
            return -1;

        int64_t n = (int64_t) rep.words[1];
        if (n > (int64_t) len)
            n = len;
        if (n > 0)
            memcpy(buf, &rep.words[2], n);
        return n;
    }
}

int64_t tty_server_write(const void *buf, uint64_t len)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t chunk = len - done;

        if (chunk > TTY_INLINE_MAX)
            chunk = TTY_INLINE_MAX;

        ipc_msg_t req;
        ipc_msg_t rep;

        memset(&req, 0, sizeof(req));
        req.tag = TTY_WRITE;
        req.words[0] = chunk;
        memcpy(&req.words[2], (const uint8_t *) buf + done, chunk);

        if (!router_forward(SVC_TTY, &req, &rep))
            return -1;
        if ((int64_t) rep.words[0] < 0)
            return -1;

        int64_t n = (int64_t) rep.words[1];
        if (n <= 0)
            break;
        done += (uint64_t) n;
    }
    return (int64_t) done;
}
