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
#define NET_MAX_SOCKS   8
#define NET_RBUF        1024

/**
 * @brief A datagram socket and its receive queue
 */
typedef struct {
    bool used;
    bool bound;
    bool stream;                /* SOCK_STREAM (TCP) when true */
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
    /* TCP state. */
    int32_t tstate;                 /* 0 closed, 1 syn sent, 2 established */
    uint32_t snd_nxt;
    uint32_t snd_una;
    uint32_t rcv_nxt;
    uint32_t peer_ip;
    uint16_t peer_port;
    bool got_ack;
    bool got_fin;
    bool listening;
    int32_t accept_pending;         /* completed connection fd, or 0 */
    int64_t acc_wait_reply;     /* deferred accept reply endpoint */
} net_sock_t;

static net_sock_t socks[NET_MAX_SOCKS];
static bootinfo_t bi;
static bool msg_deferred;

/* --- e1000e NIC driver (polling) ----------------------------------------- */

#define E1000_CTRL      0x0000
#define E1000_STATUS    0x0008
#define E1000_CTRL_EXT  0x0018
#define E1000_IMC       0x00D8
#define E1000_RCTL      0x0100
#define E1000_TCTL      0x0400
#define E1000_TIPG      0x0410
#define E1000_RDBAL     0x2800
#define E1000_RDBAH     0x2804
#define E1000_RDLEN     0x2808
#define E1000_RDH       0x2810
#define E1000_RDT       0x2818
#define E1000_TDBAL     0x3800
#define E1000_TDBAH     0x3804
#define E1000_TDLEN     0x3808
#define E1000_TDH       0x3810
#define E1000_TDT       0x3818
#define E1000_RAL0      0x5400
#define E1000_RAH0      0x5404

#define E1000_CTRL_RST  0x04000000U
#define E1000_RCTL_EN   0x00000002U
#define E1000_RCTL_UPE  0x00000008U
#define E1000_RCTL_BAM  0x00008000U
#define E1000_TCTL_EN   0x00000002U
#define E1000_TCTL_PSP  0x00000008U
#define E1000_TCTL_VAL  0x0004010AU

#define RX_COUNT        32
#define TX_COUNT        32
#define NIC_BUF         2048

typedef struct[[gnu::packed]] {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t status;
    uint8_t errors;
    uint16_t special;
} nic_rx_desc_t;

typedef struct[[gnu::packed]] {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} nic_tx_desc_t;

typedef struct[[gnu::packed]] {
    uint8_t dst[6];
    uint8_t src[6];
    uint16_t type;
} eth_hdr_t;

typedef struct[[gnu::packed]] {
    uint8_t ver_ihl;
    uint8_t tos;
    uint16_t tot_len;
    uint16_t id;
    uint16_t frag;
    uint8_t ttl;
    uint8_t proto;
    uint16_t csum;
    uint32_t src;
    uint32_t dst;
} ip_hdr_t;

typedef struct[[gnu::packed]] {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

typedef struct[[gnu::packed]] {
    uint16_t htype;
    uint16_t ptype;
    uint8_t hlen;
    uint8_t plen;
    uint16_t op;
    uint8_t sha[6];
    uint32_t spa;
    uint8_t tha[6];
    uint32_t tpa;
} arp_hdr_t;

typedef struct[[gnu::packed]] {
    uint16_t src;
    uint16_t dst;
    uint16_t len;
    uint16_t csum;
} udp_hdr_t;

typedef struct[[gnu::packed]] {
    uint16_t src;
    uint16_t dst;
    uint32_t seq;
    uint32_t ack;
    uint8_t off;
    uint8_t flags;
    uint16_t win;
    uint16_t csum;
    uint16_t urg;
} tcp_hdr_t;

#define TCP_FIN     0x01
#define TCP_SYN     0x02
#define TCP_RST     0x04
#define TCP_PSH     0x08
#define TCP_ACK     0x10

#define ARP_CACHE_N 8

typedef struct {
    bool used;
    uint32_t ip;
    uint8_t mac[6];
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_N];
static uint16_t ip_id;
static uint32_t icmp_last_seq;
static bool icmp_got_reply;


#define DMA_VADDR       0x40000000
#define DMA_SIZE        (RX_COUNT * 16 + TX_COUNT * 16 \
                         + RX_COUNT * NIC_BUF + TX_COUNT * NIC_BUF)
#define MY_IP           0x0a00020fU     /* 10.0.2.15 */
#define GW_IP           0x0a000202U     /* 10.0.2.2 */

static volatile uint32_t *nic;
static uint8_t *dma;
static uint64_t dma_phys;
static nic_rx_desc_t *rx_ring;
static nic_tx_desc_t *tx_ring;
static uint8_t *rx_bufs;
static uint8_t *tx_bufs;
static uint64_t rx_ring_phys;
static uint64_t tx_ring_phys;
static uint64_t rx_bufs_phys;
static uint64_t tx_bufs_phys;
static uint32_t rx_cur;
static uint32_t tx_cur;
static uint8_t mac[6];
static bool nic_ok;

static void net_log(const char *s)
{
    sys_serial_write(s, strlen(s));
}

static void nic_barrier(void)
{
    asm volatile ("mfence" ::: "memory");
}

/* Approximate a short delay while polling for a host round trip. */
static void net_delay(void)
{
    for (volatile uint32_t i = 0; i < 1000000; i++)
        ;
}

static void nic_write(uint32_t reg, uint32_t val)
{
    nic[reg / 4] = val;
}

static uint32_t nic_read(uint32_t reg)
{
    return nic[reg / 4];
}

static uint16_t ntohs(uint16_t v)
{
    return (uint16_t) ((v >> 8) | (v << 8));
}

static uint32_t ntohl(uint32_t v)
{
    return ((v & 0xffU) << 24) | ((v & 0xff00U) << 8)
        | ((v >> 8) & 0xff00U) | ((v >> 24) & 0xffU);
}

static uint16_t inet_csum(const void *data, uint32_t len)
{
    const uint8_t *p = data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += (uint16_t) ((p[0] << 8) | p[1]);
        p += 2;
        len -= 2;
    }
    if (len != 0)
        sum += (uint16_t) (p[0] << 8);
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t) (~sum);
}

static bool nic_send(const uint8_t *pkt, uint32_t len)
{
    nic_tx_desc_t *d = &tx_ring[tx_cur];
    uint8_t *buf = tx_bufs + tx_cur * NIC_BUF;
    uint32_t flen = (len < 60) ? 60 : len;      /* Ethernet minimum */

    if (!(d->status & 0x1))
        return false;

    memcpy(buf, pkt, len);
    if (flen > len)
        memset(buf + len, 0, flen - len);
    d->addr = tx_bufs_phys + tx_cur * NIC_BUF;
    d->length = (uint16_t) flen;
    d->cmd = 0x0b;              /* EOP | IFCS | RS */
    d->status = 0;
    tx_cur = (tx_cur + 1) % TX_COUNT;
    nic_barrier();
    nic_write(E1000_TDT, tx_cur);
    return true;
}

static int32_t nic_poll(uint8_t *out)
{
    nic_rx_desc_t *d = &rx_ring[rx_cur];

    if (!(d->status & 0x1))
        return 0;

    uint32_t len = d->length;

    if (len > NIC_BUF)
        len = NIC_BUF;
    memcpy(out, rx_bufs + rx_cur * NIC_BUF, len);
    d->status = 0;
    nic_barrier();
    nic_write(E1000_RDT, rx_cur);
    rx_cur = (rx_cur + 1) % RX_COUNT;
    return (int32_t) len;
}

static bool nic_init(void)
{
    if (bi.net_mmio_vaddr == 0 || bi.net_mmio_size == 0)
        return false;

    nic = (volatile uint32_t *) (uint64_t) bi.net_mmio_vaddr;

    if (bi.net_dma_vaddr == 0 || bi.net_dma_size < DMA_SIZE)
        return false;
    dma = (uint8_t *) (uint64_t) bi.net_dma_vaddr;
    dma_phys = bi.net_dma_phys;
    memset(dma, 0, DMA_SIZE);

    uint64_t off = 0;

    rx_ring = (nic_rx_desc_t *) dma;
    rx_ring_phys = dma_phys + off;
    off += RX_COUNT * 16;
    tx_ring = (nic_tx_desc_t *) (dma + off);
    tx_ring_phys = dma_phys + off;
    off += TX_COUNT * 16;
    rx_bufs = dma + off;
    rx_bufs_phys = dma_phys + off;
    off += RX_COUNT * NIC_BUF;
    tx_bufs = dma + off;
    tx_bufs_phys = dma_phys + off;

    for (int32_t i = 0; i < RX_COUNT; i++) {
        rx_ring[i].addr = rx_bufs_phys + i * NIC_BUF;
        rx_ring[i].status = 0;
    }
    for (int32_t i = 0; i < TX_COUNT; i++) {
        tx_ring[i].addr = tx_bufs_phys + i * NIC_BUF;
        tx_ring[i].status = 0x1;        /* free */
    }

    nic_write(E1000_IMC, 0xffffffffU);  /* no interrupts, poll */
    nic_write(E1000_CTRL, nic_read(E1000_CTRL) | E1000_CTRL_RST);
    for (int32_t i = 0; i < 100000 && (nic_read(E1000_CTRL) & E1000_CTRL_RST); i++)
        ;

    /* Set Link Up so the MAC will transmit. */
    nic_write(E1000_CTRL, nic_read(E1000_CTRL) | 0x00000040U);

    uint32_t ral = nic_read(E1000_RAL0);
    uint32_t rah = nic_read(E1000_RAH0);

    mac[0] = ral & 0xff;
    mac[1] = (ral >> 8) & 0xff;
    mac[2] = (ral >> 16) & 0xff;
    mac[3] = (ral >> 24) & 0xff;
    mac[4] = rah & 0xff;
    mac[5] = (rah >> 8) & 0xff;

    /* Rings. */
    nic_write(E1000_RDBAL, (uint32_t) rx_ring_phys);
    nic_write(E1000_RDBAH, (uint32_t) (rx_ring_phys >> 32));
    nic_write(E1000_RDLEN, RX_COUNT * 16);
    nic_write(E1000_RDH, 0);
    nic_write(E1000_RDT, RX_COUNT - 1);

    nic_write(E1000_TDBAL, (uint32_t) tx_ring_phys);
    nic_write(E1000_TDBAH, (uint32_t) (tx_ring_phys >> 32));
    nic_write(E1000_TDLEN, TX_COUNT * 16);
    nic_write(E1000_TDH, 0);
    nic_write(E1000_TDT, 0);

    nic_write(E1000_TIPG, 0x0060200aU);
    nic_write(E1000_RCTL, E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_BAM
              | 0x04000000U);   /* SECRC: strip the FCS */
    nic_write(E1000_TCTL, E1000_TCTL_VAL);

    rx_cur = 0;
    tx_cur = 0;
    nic_ok = true;
    return true;
}

static void net_rx_frame(uint8_t * pkt, int32_t n);
static bool ip_send(uint32_t dst, uint8_t proto, const uint8_t * payload,
                    uint32_t plen);

static void arp_store(uint32_t ip, const uint8_t * m)
{
    for (int32_t i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(arp_cache[i].mac, m, 6);
            return;
        }
    }
    for (int32_t i = 0; i < ARP_CACHE_N; i++) {
        if (!arp_cache[i].used) {
            arp_cache[i].used = true;
            arp_cache[i].ip = ip;
            memcpy(arp_cache[i].mac, m, 6);
            return;
        }
    }
}

static bool arp_lookup(uint32_t ip, uint8_t * out)
{
    for (int32_t i = 0; i < ARP_CACHE_N; i++) {
        if (arp_cache[i].used && arp_cache[i].ip == ip) {
            memcpy(out, arp_cache[i].mac, 6);
            return true;
        }
    }
    return false;
}

/* Drain the RX ring and dispatch each frame. Bounded so a retransmit storm
 * cannot starve the request loop. */
static void net_poll_once(void)
{
    uint8_t rx[NIC_BUF];

    if (!nic_ok)
        return;

    for (int32_t i = 0; i < 16; i++) {
        int32_t n = nic_poll(rx);

        if (n <= 0)
            break;
        net_rx_frame(rx, n);
    }
}

static bool arp_resolve(uint32_t ip, uint8_t * out)
{
    if (arp_lookup(ip, out))
        return true;

    uint8_t pkt[64];
    eth_hdr_t *e = (eth_hdr_t *) pkt;
    arp_hdr_t *a = (arp_hdr_t *) (pkt + 14);

    memset(pkt, 0, sizeof(pkt));
    memset(e->dst, 0xff, 6);
    memcpy(e->src, mac, 6);
    e->type = ntohs(0x0806);
    a->htype = ntohs(1);
    a->ptype = ntohs(0x0800);
    a->hlen = 6;
    a->plen = 4;
    a->op = ntohs(1);
    memcpy(a->sha, mac, 6);
    a->spa = ntohl(MY_IP);
    a->tpa = ntohl(ip);

    if (!nic_send(pkt, 14 + sizeof(arp_hdr_t)))
        return false;

    for (int32_t i = 0; i < 4000000; i++) {
        net_poll_once();
        if (arp_lookup(ip, out))
            return true;
    }
    return false;
}

static bool icmp_ping(uint32_t ip, uint16_t seq)
{
    uint8_t seg[64];
    const char *payload = "hanos-ping";
    uint32_t plen = strlen(payload);
    uint32_t icmp_len = sizeof(icmp_hdr_t) + plen;
    icmp_hdr_t *ic = (icmp_hdr_t *) seg;

    ic->type = 8;
    ic->code = 0;
    ic->id = ntohs(0x1234);
    ic->seq = ntohs(seq);
    memcpy(seg + sizeof(icmp_hdr_t), payload, plen);
    ic->csum = 0;
    ic->csum = ntohs(inet_csum(ic, icmp_len));

    icmp_got_reply = false;
    if (!ip_send(ip, 1, seg, icmp_len))
        return false;

    for (int32_t i = 0; i < 4000000; i++) {
        net_poll_once();
        if (icmp_got_reply && icmp_last_seq == seq)
            return true;
    }
    return false;
}

static void nic_selftest(void)
{
    if (!nic_ok) {
        net_log("net: no NIC (loopback only)\n");
        return;
    }

    net_log("net: NIC up\n");

    uint8_t gw_mac[6];

    if (!arp_resolve(GW_IP, gw_mac)) {
        net_log("net: ARP gateway FAIL\n");
        return;
    }
    net_log("net: ARP gateway ok\n");

    if (icmp_ping(GW_IP, 1))
        net_log("net: ping 10.0.2.2 ok\n");
    else
        net_log("net: ping 10.0.2.2 FAIL\n");
}

static net_sock_t *sock_get(int32_t fd)
{
    if (fd < 1 || fd > NET_MAX_SOCKS || !socks[fd - 1].used)
        return NULL;
    return &socks[fd - 1];
}

static int32_t sock_alloc(void)
{
    for (int32_t i = 0; i < NET_MAX_SOCKS; i++) {
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

static bool ip_send(uint32_t dst, uint8_t proto, const uint8_t * payload,
                    uint32_t plen)
{
    uint8_t dmac[6];

    if (!arp_resolve(dst, dmac))
        return false;
    if (plen > 1400)
        return false;

    uint8_t pkt[14 + 20 + 1500];
    eth_hdr_t *e = (eth_hdr_t *) pkt;
    ip_hdr_t *ip4 = (ip_hdr_t *) (pkt + 14);

    memcpy(e->dst, dmac, 6);
    memcpy(e->src, mac, 6);
    e->type = ntohs(0x0800);
    ip4->ver_ihl = 0x45;
    ip4->tos = 0;
    ip4->tot_len = ntohs((uint16_t) (20 + plen));
    ip4->id = ntohs(++ip_id);
    ip4->frag = 0;
    ip4->ttl = 64;
    ip4->proto = proto;
    ip4->src = ntohl(MY_IP);
    ip4->dst = ntohl(dst);
    ip4->csum = 0;
    ip4->csum = ntohs(inet_csum(ip4, 20));
    memcpy(pkt + 34, payload, plen);
    nic_barrier();
    return nic_send(pkt, 14 + 20 + plen);
}

static bool udp_send(net_sock_t * s, uint32_t dst, uint16_t dport,
                     const uint8_t * data, uint32_t len)
{
    if (len > 1400)
        return false;

    uint8_t seg[8 + 1500];
    udp_hdr_t *u = (udp_hdr_t *) seg;
    uint16_t sport;

    if (!s->bound) {
        s->bound = true;
        s->port = (uint16_t) (40000 + (s - socks));
    }
    sport = s->port;

    u->src = ntohs(sport);
    u->dst = ntohs(dport);
    u->len = ntohs((uint16_t) (8 + len));
    u->csum = 0;
    memcpy(seg + 8, data, len);
    return ip_send(dst, 17, seg, 8 + len);
}

/* Send one TCP segment. The pseudo-header is prepended for the checksum. */
static bool tcp_send_seg(net_sock_t * s, uint8_t flags, const uint8_t * data,
                         uint32_t len)
{
    uint8_t buf[12 + 20 + 1400];
    tcp_hdr_t *t = (tcp_hdr_t *) (buf + 12);
    uint16_t sport = s->bound ? s->port : (uint16_t) (40000 + (s - socks));
    uint32_t mybe = ntohl(MY_IP);
    uint32_t pbe = ntohl(s->peer_ip);
    uint16_t total = (uint16_t) (20 + len);
    uint16_t total_be = ntohs(total);

    memcpy(buf + 0, &mybe, 4);
    memcpy(buf + 4, &pbe, 4);
    buf[8] = 0;
    buf[9] = 6;
    memcpy(buf + 10, &total_be, 2);

    memset(t, 0, 20);
    t->src = ntohs(sport);
    t->dst = ntohs(s->peer_port);
    t->seq = ntohl(s->snd_nxt);
    t->ack = ntohl(s->rcv_nxt);
    t->off = 5 << 4;
    t->flags = flags;
    t->win = ntohs(2048);
    if (len > 0)
        memcpy(buf + 12 + 20, data, len);

    t->csum = 0;
    t->csum = ntohs(inet_csum(buf, 12 + 20 + len));
    return ip_send(s->peer_ip, 6, (const uint8_t *) t, 20 + len);
}

static bool tcp_connect(net_sock_t * s, uint32_t ip, uint16_t port)
{
    s->peer_ip = ip;
    s->peer_port = port;
    s->bound = true;
    s->port = (uint16_t) (40000 + (s - socks));
    s->snd_nxt = 1000 + (uint32_t) (s - socks) * 100;
    s->snd_una = s->snd_nxt;
    s->rcv_nxt = 0;
    s->tstate = 1;

    if (!tcp_send_seg(s, TCP_SYN, NULL, 0))
        return false;
    s->snd_nxt++;

    for (int32_t i = 0; i < 3000; i++) {
        net_poll_once();
        if (s->tstate == 2) {
            tcp_send_seg(s, TCP_ACK, NULL, 0);
            return true;
        }
        if (s->tstate < 0)
            return false;
        net_delay();
    }
    return false;
}

static int32_t tcp_send(net_sock_t * s, const uint8_t * data, uint32_t len)
{
    if (s->tstate != 2)
        return -1;
    if (len > 1400)
        len = 1400;
    if (!tcp_send_seg(s, TCP_ACK | TCP_PSH, data, len))
        return -1;

    s->snd_nxt += len;
    return (int32_t) len;
}

/* Find a TCP socket for an incoming segment: match the peer first, then fall
 * back to a listening socket on the local port. */
static net_sock_t *tcp_find(uint16_t dport, uint32_t src, uint16_t sport,
                            bool * is_listen)
{
    net_sock_t *ls = NULL;

    *is_listen = false;
    for (int32_t i = 0; i < NET_MAX_SOCKS; i++) {
        net_sock_t *s = &socks[i];

        if (!s->used || !s->stream || !s->bound || s->port != dport)
            continue;
        if (s->listening) {
            ls = s;
            continue;
        }
        if (s->peer_ip == src && s->peer_port == sport)
            return s;
    }
    if (ls != NULL)
        *is_listen = true;
    return ls;
}

/* Dispatch one received Ethernet frame: ARP, ICMP, UDP and TCP. */
static void net_rx_frame(uint8_t * pkt, int32_t n)
{
    if (n < 14)
        return;

    eth_hdr_t *e = (eth_hdr_t *) pkt;
    uint16_t type = ntohs(e->type);

    if (type == 0x0806 && n >= 42) {
        arp_hdr_t *a = (arp_hdr_t *) (pkt + 14);

        if (ntohs(a->op) == 2) {
            arp_store(ntohl(a->spa), a->sha);
        } else if (ntohs(a->op) == 1 && ntohl(a->tpa) == MY_IP) {
            uint8_t r[42];
            eth_hdr_t *re = (eth_hdr_t *) r;
            arp_hdr_t *ra = (arp_hdr_t *) (r + 14);

            memcpy(re->dst, a->sha, 6);
            memcpy(re->src, mac, 6);
            re->type = ntohs(0x0806);
            ra->htype = ntohs(1);
            ra->ptype = ntohs(0x0800);
            ra->hlen = 6;
            ra->plen = 4;
            ra->op = ntohs(2);
            memcpy(ra->sha, mac, 6);
            ra->spa = ntohl(MY_IP);
            memcpy(ra->tha, a->sha, 6);
            ra->tpa = a->spa;
            nic_send(r, 42);
        }
        return;
    }

    if (type != 0x0800 || n < 34)
        return;

    ip_hdr_t *ip4 = (ip_hdr_t *) (pkt + 14);
    uint32_t src = ntohl(ip4->src);
    uint32_t ihl = (uint32_t) (ip4->ver_ihl & 0xf) * 4;
    uint32_t iptot = ntohs(ip4->tot_len);

    if (ip4->proto == 1 && ihl >= 20 && n >= 14 + (int32_t) ihl
        + (int32_t) sizeof(icmp_hdr_t)) {
        icmp_hdr_t *ic = (icmp_hdr_t *) (pkt + 14 + ihl);

        if (ic->type == 8) {
            uint32_t icmp_len = (14 + iptot > (uint32_t) n)
                ? (uint32_t) n - 14 - ihl : iptot - ihl;

            if (icmp_len > 64)
                icmp_len = 64;

            uint8_t seg[64];

            memcpy(seg, ic, icmp_len);
            icmp_hdr_t *ric = (icmp_hdr_t *) seg;

            ric->type = 0;
            ric->csum = 0;
            ric->csum = ntohs(inet_csum(ric, icmp_len));
            ip_send(src, 1, seg, icmp_len);
        } else if (ic->type == 0) {
            icmp_last_seq = ntohs(ic->seq);
            icmp_got_reply = true;
        }
        return;
    }

    if (ip4->proto == 17 && ihl >= 20
        && n >= 14 + (int32_t) ihl + (int32_t) sizeof(udp_hdr_t)) {
        udp_hdr_t *u = (udp_hdr_t *) (pkt + 14 + ihl);
        uint16_t dport = ntohs(u->dst);
        uint16_t sport = ntohs(u->src);
        uint32_t ulen = ntohs(u->len) >= 8 ? ntohs(u->len) - 8 : 0;

        if (14 + ihl + 8 + ulen > (uint32_t) n)
            ulen = (uint32_t) n - 14 - ihl - 8;

        for (int32_t i = 0; i < NET_MAX_SOCKS; i++) {
            if (addr_match(&socks[i], MY_IP, dport)) {
                sock_push(&socks[i], pkt + 14 + ihl + 8, ulen, src, sport);
                sock_flush(&socks[i]);
                break;
            }
        }
        return;
    }

    if (ip4->proto == 6 && ihl >= 20
        && n >= 14 + (int32_t) ihl + (int32_t) sizeof(tcp_hdr_t)) {
        tcp_hdr_t *t = (tcp_hdr_t *) (pkt + 14 + ihl);
        uint16_t dport = ntohs(t->dst);
        uint32_t seq = ntohl(t->seq);
        uint32_t ack = ntohl(t->ack);
        uint32_t tcp_off = (uint32_t) ((t->off >> 4) & 0xf) * 4;
        uint32_t poff = 14 + ihl + tcp_off;
        uint32_t plen = (iptot > ihl + tcp_off)
            ? iptot - ihl - tcp_off : 0;
        uint16_t sport = ntohs(t->src);
        bool is_listen = false;
        net_sock_t *s;

        if (n < (int32_t) poff)
            plen = 0;
        else if (poff + plen > (uint32_t) n)
            plen = (uint32_t) n - poff;

        s = tcp_find(dport, src, sport, &is_listen);
        if (s == NULL)
            return;

        if (is_listen) {
            if ((t->flags & TCP_SYN) && !(t->flags & TCP_ACK)) {
                int32_t nfd = sock_alloc();

                if (nfd > 0) {
                    net_sock_t *ns = &socks[nfd - 1];

                    ns->stream = true;
                    ns->bound = true;
                    ns->port = dport;
                    ns->peer_ip = src;
                    ns->peer_port = sport;
                    ns->snd_nxt = 2000 + (uint32_t) nfd * 100;
                    ns->snd_una = ns->snd_nxt;
                    ns->rcv_nxt = seq + 1;
                    ns->tstate = 2;
                    tcp_send_seg(ns, TCP_SYN | TCP_ACK, NULL, 0);
                    ns->snd_nxt++;

                    if (s->acc_wait_reply != 0) {
                        sys_ipc_msg_t rr;

                        memset(&rr, 0, sizeof(rr));
                        rr.tag = NET_ACCEPT;
                        rr.words[0] = 0;
                        rr.words[1] = (uint64_t) nfd;
                        sys_ipc_send(s->acc_wait_reply, &rr);
                        sys_handle_close(s->acc_wait_reply);
                        s->acc_wait_reply = 0;
                    } else {
                        s->accept_pending = nfd;
                    }
                }
            }
            return;
        }

        if (t->flags & TCP_RST) {
            s->tstate = -1;
            return;
        }
        if ((t->flags & TCP_SYN) && (t->flags & TCP_ACK)
            && s->tstate == 1) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->tstate = 2;
            return;
        }
        if (t->flags & TCP_ACK) {
            if (ack > s->snd_una) {
                s->snd_una = ack;
            }
        }
        if (plen > 0) {
            if (seq == s->rcv_nxt) {
                sock_push(s, pkt + poff, plen, src, ntohs(t->src));
                s->rcv_nxt = seq + plen;
                sock_flush(s);
            }
            tcp_send_seg(s, TCP_ACK, NULL, 0);
        }
        if (t->flags & TCP_FIN) {
            s->rcv_nxt = seq + 1;
            tcp_send_seg(s, TCP_ACK, NULL, 0);
            s->got_fin = true;
        }
        return;
    }
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == NET_PING) {
        rep->words[0] = NET_PING;
        return;
    }

    if (m->tag == NET_SOCKET) {
        uint64_t type = m->words[1];

        if (m->words[0] != AF_INET
            || (type != SOCK_DGRAM && type != SOCK_STREAM)) {
            rep->words[0] = (uint64_t) (int64_t) -97;   /* -EAFNOSUPPORT */
            return;
        }
        int32_t fd = sock_alloc();

        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            return;
        }
        socks[fd - 1].stream = (type == SOCK_STREAM);
        rep->words[0] = 0;
        rep->words[1] = (uint64_t) fd;
        return;
    }

    if (m->tag == NET_CONNECT) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL || !s->stream) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }
        if (!nic_ok
            || !tcp_connect(s, (uint32_t) m->words[1],
                            (uint16_t) m->words[2])) {
            rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
            return;
        }
        rep->words[0] = 0;
        return;
    }

    if (m->tag == NET_LISTEN) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL || !s->stream || !s->bound) {
            rep->words[0] = (uint64_t) (int64_t) -22;
            return;
        }
        s->listening = true;
        rep->words[0] = 0;
        return;
    }

    if (m->tag == NET_ACCEPT) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL || !s->listening) {
            rep->words[0] = (uint64_t) (int64_t) -22;
            return;
        }
        if (s->accept_pending > 0) {
            int32_t fd = s->accept_pending;

            s->accept_pending = 0;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) fd;
            return;
        }
        if (s->acc_wait_reply != 0) {
            /* One held accept per socket; answer the previous one. */
            sys_ipc_msg_t old;

            memset(&old, 0, sizeof(old));
            old.tag = NET_ACCEPT;
            old.words[0] = (uint64_t) (int64_t) -11;    /* -EAGAIN */
            sys_ipc_send(s->acc_wait_reply, &old);
            sys_handle_close(s->acc_wait_reply);
        }
        s->acc_wait_reply = (int64_t) m->xfer[0];
        msg_deferred = true;
        return;
    }

    if (m->tag == NET_BIND) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);
        uint32_t ip = (uint32_t) m->words[1];
        uint16_t port = (uint16_t) m->words[2];

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }
        for (int32_t i = 0; i < NET_MAX_SOCKS; i++) {
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

    if (m->tag == NET_GETSOCKNAME) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }
        rep->words[0] = 0;
        rep->words[1] = s->ip;
        rep->words[2] = s->port;
        return;
    }

    if (m->tag == NET_GETPEERNAME) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }
        rep->words[0] = 0;
        rep->words[1] = s->peer_ip;
        rep->words[2] = s->peer_port;
        return;
    }

    if (m->tag == NET_POLL) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        bool readable = (s->count > 0)
            || (s->accept_pending > 0)
            || (s->stream && s->got_fin);
        bool writable = s->stream ? (s->tstate == 2) : true;

        rep->words[0] = 0;
        rep->words[1] = readable ? 1 : 0;
        rep->words[2] = writable ? 1 : 0;
        return;
    }

    if (m->tag == NET_SENDTO) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);
        uint32_t ip = (uint32_t) m->words[1];
        uint16_t port = (uint16_t) m->words[2];
        uint32_t len = (uint32_t) m->words[3];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t pkt[NET_RBUF];

        if (s == NULL || memh == 0 || len > NET_RBUF) {
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

        if (s->stream) {
            int32_t n = tcp_send(s, pkt, len);

            if (n < 0) {
                rep->words[0] = (uint64_t) (int64_t) -5;        /* -EIO */
                return;
            }
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) n;
            return;
        }

        if (is_loopback(ip)) {
            for (int32_t i = 0; i < NET_MAX_SOCKS; i++) {
                if (addr_match(&socks[i], ip, port)) {
                    sock_push(&socks[i], pkt, len, s->ip, s->port);
                    sock_flush(&socks[i]);
                    break;
                }
            }
        } else if (nic_ok) {
            udp_send(s, ip, port, pkt, len);
        }

        rep->words[0] = 0;
        rep->words[1] = len;
        return;
    }

    if (m->tag == NET_RECVFROM) {
        net_sock_t *s = sock_get((int32_t) m->words[0]);
        uint32_t len = (uint32_t) m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (s == NULL || memh == 0) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }

        if (s->count == 0) {
            /* One held read per socket. Answer a previous one so a second
             * reader cannot leak its reply endpoint. */
            if (s->wait_reply != 0) {
                sys_ipc_msg_t old;

                memset(&old, 0, sizeof(old));
                old.tag = NET_RECVFROM;
                old.words[0] = (uint64_t) (int64_t) -11;        /* -EAGAIN */
                sys_ipc_send(s->wait_reply, &old);
                sys_handle_close(s->wait_reply);
                sys_handle_close(s->wait_memh);
            }
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
        net_sock_t *s = sock_get((int32_t) m->words[0]);

        if (s == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            return;
        }
        if (s->stream && s->tstate == 2) {
            tcp_send_seg(s, TCP_FIN | TCP_ACK, NULL, 0);
            s->snd_nxt++;
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

int32_t main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    nic_init();
    nic_selftest();

    for (;;) {
        net_poll_once();

        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 10) != 0)
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
