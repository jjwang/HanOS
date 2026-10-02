/**-----------------------------------------------------------------------------

 @file    net.c
 @brief   Userspace network server

 @details
 @verbatim

   Owns the network stack. This first cut serves AF_INET datagram sockets and
   loops traffic back inside the server: a datagram sent to the loopback
   address is delivered to the socket bound to that ip and port. A recvfrom
   with no data is held and answered when a datagram arrives.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

#define NET_BUF_ADDR    0x20000000
#define NET_MAX_SOCKS   32
#define NET_RBUF        4096

/**
 * @brief A datagram socket and its receive queue
 */
typedef struct {
    bool used;
    bool bound;
    uint32_t ip;
    uint16_t port;
    uint8_t rbuf[NET_RBUF];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint32_t src_ip;
    uint16_t src_port;
    int64_t wait_reply;
    int64_t wait_memh;
    uint32_t wait_len;
} net_sock_t;

static net_sock_t socks[NET_MAX_SOCKS];
static bootinfo_t bi;
static bool msg_deferred;

static net_sock_t *sock_get(int fd)
{
    if (fd < 1 || fd > NET_MAX_SOCKS || !socks[fd - 1].used)
        return NULL;
    return &socks[fd - 1];
}

static int sock_alloc(void)
{
    for (int i = 0; i < NET_MAX_SOCKS; i++) {
        if (socks[i].used)
            continue;
        memset(&socks[i], 0, sizeof(socks[i]));
        socks[i].used = true;
        return i + 1;
    }
    return -1;
}

static bool is_loopback(uint32_t ip)
{
    return ip == 0 || ip == NET_IP_LOOPBACK;
}

static bool addr_match(const net_sock_t * s, uint32_t ip, uint16_t port)
{
    if (!s->used || !s->bound || s->port != port)
        return false;
    return s->ip == ip || s->ip == 0 || ip == 0;
}

/* Copy n bytes out of the socket queue into a mapped buffer. */
static uint32_t sock_pop(net_sock_t * s, uint8_t * buf, uint32_t n)
{
    uint32_t total = (n < s->count) ? n : s->count;

    for (uint32_t i = 0; i < total; i++) {
        buf[i] = s->rbuf[s->tail];
        s->tail = (s->tail + 1) % NET_RBUF;
    }
    s->count -= total;
    return total;
}

static void sock_push(net_sock_t * s, const uint8_t * data, uint32_t len,
                      uint32_t src_ip, uint16_t src_port)
{
    s->src_ip = src_ip;
    s->src_port = src_port;

    for (uint32_t i = 0; i < len && s->count < NET_RBUF; i++) {
        s->rbuf[s->head] = data[i];
        s->head = (s->head + 1) % NET_RBUF;
        s->count++;
    }
}

/* Answer a held recvfrom if the queue has data. */
static void sock_flush(net_sock_t * s)
{
    if (s->wait_reply == 0 || s->count == 0)
        return;

    uint32_t n = 0;

    if (sys_mem_map(s->wait_memh, NET_BUF_ADDR, 3) == 0) {
        n = sock_pop(s, (uint8_t *) (uint64_t) NET_BUF_ADDR, s->wait_len);
        sys_mem_unmap(s->wait_memh, NET_BUF_ADDR);
    }

    sys_ipc_msg_t rr;

    memset(&rr, 0, sizeof(rr));
    rr.tag = NET_RECVFROM;
    rr.words[0] = 0;
    rr.words[1] = n;
    rr.words[2] = s->src_ip;
    rr.words[3] = s->src_port;
    sys_ipc_send(s->wait_reply, &rr);
    sys_handle_close(s->wait_reply);
    sys_handle_close(s->wait_memh);
    s->wait_reply = 0;
    s->wait_memh = 0;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == NET_PING) {
        rep->words[0] = NET_PING;
        return;
    }

    if (m->tag == NET_SOCKET) {
        if (m->words[0] != AF_INET || m->words[1] != SOCK_DGRAM) {
            rep->words[0] = (uint64_t) (int64_t) -97;   /* -EAFNOSUPPORT */
            return;
        }
        int fd = sock_alloc();
        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            return;
        }
        rep->words[0] = 0;
        rep->words[1] = (uint64_t) fd;
        return;
    }

    if (m->tag == NET_BIND) {
        net_sock_t *s = sock_get((int) m->words[0]);
        uint32_t ip = (uint32_t) m->words[1];
        uint16_t port = (uint16_t) m->words[2];

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }
        for (int i = 0; i < NET_MAX_SOCKS; i++) {
            if (&socks[i] != s && addr_match(&socks[i], ip, port)) {
                rep->words[0] = (uint64_t) (int64_t) -98;       /* -EADDRINUSE */
                return;
            }
        }
        s->ip = ip;
        s->port = port;
        s->bound = true;
        rep->words[0] = 0;
        return;
    }

    if (m->tag == NET_SENDTO) {
        net_sock_t *s = sock_get((int) m->words[0]);
        uint32_t ip = (uint32_t) m->words[1];
        uint16_t port = (uint16_t) m->words[2];
        uint32_t len = (uint32_t) m->words[3];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t pkt[NET_RBUF];

        if (s == NULL || memh == 0 || len > NET_RBUF || !is_loopback(ip)) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -22;   /* -EINVAL */
            return;
        }
        if (len > 0 && sys_mem_map(memh, NET_BUF_ADDR, 1) != 0) {
            sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }
        if (len > 0) {
            memcpy(pkt, (const void *) (uint64_t) NET_BUF_ADDR, len);
            sys_mem_unmap(memh, NET_BUF_ADDR);
        }
        sys_handle_close(memh);

        for (int i = 0; i < NET_MAX_SOCKS; i++) {
            if (addr_match(&socks[i], ip, port)) {
                sock_push(&socks[i], pkt, len, s->ip, s->port);
                sock_flush(&socks[i]);
                break;
            }
        }

        rep->words[0] = 0;
        rep->words[1] = len;
        return;
    }

    if (m->tag == NET_RECVFROM) {
        net_sock_t *s = sock_get((int) m->words[0]);
        uint32_t len = (uint32_t) m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (s == NULL || memh == 0) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }

        if (s->count == 0) {
            s->wait_reply = (int64_t) m->xfer[0];
            s->wait_memh = memh;
            s->wait_len = len;
            msg_deferred = true;
            return;
        }

        uint32_t n = 0;

        if (sys_mem_map(memh, NET_BUF_ADDR, 3) == 0) {
            n = sock_pop(s, (uint8_t *) (uint64_t) NET_BUF_ADDR, len);
            sys_mem_unmap(memh, NET_BUF_ADDR);
        }
        sys_handle_close(memh);

        rep->words[0] = 0;
        rep->words[1] = n;
        rep->words[2] = s->src_ip;
        rep->words[3] = s->src_port;
        return;
    }

    if (m->tag == NET_CLOSE) {
        net_sock_t *s = sock_get((int) m->words[0]);

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }
        if (s->wait_reply != 0) {
            sys_ipc_msg_t rr;

            memset(&rr, 0, sizeof(rr));
            rr.tag = NET_RECVFROM;
            rr.words[0] = (uint64_t) (int64_t) -9;
            sys_ipc_send(s->wait_reply, &rr);
            sys_handle_close(s->wait_reply);
            sys_handle_close(s->wait_memh);
        }
        s->used = false;
        rep->words[0] = 0;
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
