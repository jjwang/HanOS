/**-----------------------------------------------------------------------------

 @file    net_srv.c
 @brief   Spawn the userspace network server
 @details
 @verbatim

  Creates the service endpoint, spawns /bin/net and registers it with the
  router as SVC_NET. The socket calls below forward to it, moving data through
  a memory object.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>
#include <bootinfo.h>
#include <protocol.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <mm/mm.h>
#include <mm/memobj.h>
#include <ipc/object.h>
#include <ipc/ipc.h>
#include <router/router.h>
#include <srv/net_srv.h>
#include <proc/sched.h>

#define NET_IO_BUF_SIZE     VFS_IO_BUF_SIZE

static endpoint_t *net_ep = NULL;
static pid_t net_spawner = PID_MAX;
static bool net_active = false;

static void net_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != net_spawner || net_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(net_ep),
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

    klogi("net: attached service endpoint to pid %ld\n", (long) tc->pid);
}

bool net_server_start(void)
{
    net_ep = endpoint_create();
    if (net_ep == NULL)
        return false;

    const char *argv[] = { "net", NULL };

    net_spawner = sched_get_pid();
    sched_set_spawn_hook(net_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_NET_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_NET, net_ep, tc->pid);
    net_active = true;
    klogi("net: server started and registered as SVC_NET\n");
    return true;
}

bool net_server_active(void)
{
    return net_active;
}

/* --- socket calls into the network server -------------------------------- */

static memobj_t *net_memobj(uint64_t len, handle_t * out)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || len == 0)
        return NULL;

    memobj_t *mo = memobj_create(len);
    if (mo == NULL)
        return NULL;

    handle_t h = handle_alloc(&t->handles, memobj_object(mo),
                              HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                              | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        memobj_unref(mo);
        return NULL;
    }

    *out = h;
    return mo;
}

static void net_copy_in(memobj_t * mo, const void *src, uint64_t len)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t pos = done;
        uint64_t chunk = PAGE_SIZE - (pos & (PAGE_SIZE - 1));

        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *) PHYS_TO_VIRT(memobj_page(mo, pos / PAGE_SIZE))
               + (pos & (PAGE_SIZE - 1)), (const uint8_t *) src + done,
               chunk);
        done += chunk;
    }
}

static void net_copy_out(memobj_t * mo, void *dst, uint64_t len)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t pos = done;
        uint64_t chunk = PAGE_SIZE - (pos & (PAGE_SIZE - 1));

        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *) dst + done,
               (uint8_t *) PHYS_TO_VIRT(memobj_page(mo, pos / PAGE_SIZE))
               + (pos & (PAGE_SIZE - 1)), chunk);
        done += chunk;
    }
}

int64_t net_socket(int domain, int type, int protocol)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_SOCKET;
    req.words[0] = (uint64_t) domain;
    req.words[1] = (uint64_t) type;
    req.words[2] = (uint64_t) protocol;

    if (!router_forward(SVC_NET, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;
    return (int64_t) rep.words[1];
}

int64_t net_bind(int sock, uint32_t ip, uint16_t port)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_BIND;
    req.words[0] = (uint64_t) sock;
    req.words[1] = (uint64_t) ip;
    req.words[2] = (uint64_t) port;

    if (!router_forward(SVC_NET, &req, &rep))
        return -1;
    return (int64_t) rep.words[0];
}

int64_t net_sendto(int sock, uint32_t ip, uint16_t port, const void *buf,
                   uint64_t len)
{
    if (len > NET_IO_BUF_SIZE)
        len = NET_IO_BUF_SIZE;

    handle_t mh = 0;
    memobj_t *mo = NULL;

    if (len > 0) {
        mo = net_memobj(len, &mh);
        if (mo == NULL)
            return -1;
        net_copy_in(mo, buf, len);
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_SENDTO;
    req.words[0] = (uint64_t) sock;
    req.words[1] = (uint64_t) ip;
    req.words[2] = (uint64_t) port;
    req.words[3] = len;
    if (mo != NULL) {
        req.xfer[0] = mh;
        req.xfer_count = 1;
    }

    if (!router_forward(SVC_NET, &req, &rep) || (int64_t) rep.words[0] < 0) {
        if (mo != NULL)
            memobj_unref(mo);
        return -1;
    }

    int64_t n = (int64_t) rep.words[1];

    if (mo != NULL)
        memobj_unref(mo);
    return n;
}

int64_t net_recvfrom(int sock, void *buf, uint64_t len, uint32_t *ip,
                     uint16_t *port)
{
    if (len > NET_IO_BUF_SIZE)
        len = NET_IO_BUF_SIZE;

    handle_t mh = 0;
    memobj_t *mo = net_memobj(len ? len : 1, &mh);

    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_RECVFROM;
    req.words[0] = (uint64_t) sock;
    req.words[1] = len;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward_timeout(SVC_NET, &req, &rep, 3600 * 1000)
        || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    int64_t n = (int64_t) rep.words[1];

    if (n > (int64_t) len)
        n = len;
    if (n > 0)
        net_copy_out(mo, buf, (uint64_t) n);
    if (ip != NULL)
        *ip = (uint32_t) rep.words[2];
    if (port != NULL)
        *port = (uint16_t) rep.words[3];

    memobj_unref(mo);
    return n;
}

int64_t net_close(int sock)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_CLOSE;
    req.words[0] = (uint64_t) sock;

    if (!router_forward(SVC_NET, &req, &rep))
        return -1;
    return (int64_t) rep.words[0];
}
