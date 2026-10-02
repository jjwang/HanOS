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
#include <arch/x64/pci.h>

#define NET_IO_BUF_SIZE     VFS_IO_BUF_SIZE

/* Where the NIC MMIO window is mapped in the network server's address space. */
#define NET_MMIO_VADDR      0x30000000UL
#define NET_DMA_VADDR       0x40000000UL
#define NET_DMA_SIZE        (256 * 1024)

static endpoint_t *net_ep = NULL;
static pid_t net_spawner = PID_MAX;
static bool net_active = false;

/* Grant the NIC's MMIO window to the server. Returns false when no supported
 * controller is present, in which case the server keeps only loopback. */
static bool net_grant_nic(process_t * tc, bootinfo_t * bi)
{
    pci_device_t dev;
    const uint16_t vendors[] = { 0x8086, 0x8086, 0x8086 };
    const uint16_t devices[] = { 0x10d3, 0x100e, 0x100f };
    bool found = false;

    for (uint64_t i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        if (pci_find(vendors[i], devices[i], &dev)) {
            found = true;
            break;
        }
    }
    if (!found)
        return false;

    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

    /* Bus master + memory space. */
    pci_outw(id, PCI_CONFIG_COMMAND, pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

    pci_bar_t bar;

    pci_get_bar(&bar, id, 0);
    if (bar.u.address == NULL || bar.size == 0)
        return false;

    vmm_map(tc->addrspace, NET_MMIO_VADDR, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size), VMM_FLAGS_MMIO | VMM_FLAG_USER);

    bi->net_mmio_vaddr = NET_MMIO_VADDR;
    bi->net_mmio_size = bar.size;

    /* Contiguous DMA memory the server programs into the NIC rings. */
    void *dma = kmalloc_chunk(NET_DMA_SIZE, __func__, __LINE__);

    if (dma != NULL) {
        uint64_t dma_phys = VIRT_TO_PHYS((uint64_t) dma);

        vmm_map(tc->addrspace, NET_DMA_VADDR, dma_phys,
                NUM_PAGES(NET_DMA_SIZE), VMM_FLAGS_DEFAULT | VMM_FLAG_USER);
        bi->net_dma_vaddr = NET_DMA_VADDR;
        bi->net_dma_phys = dma_phys;
        bi->net_dma_size = NET_DMA_SIZE;
        klogi("net: granted DMA 0x%lx (%ld bytes) at 0x%lx\n", dma_phys,
              (long) NET_DMA_SIZE, (long) NET_DMA_VADDR);
    }

    klogi("net: granted NIC %04x:%04x BAR0 0x%lx (%ld bytes) at 0x%lx\n",
          dev.vendor_id, dev.device_id, (uint64_t) bar.u.address, bar.size,
          NET_MMIO_VADDR);
    return true;
}

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

    net_grant_nic(tc, bi);

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

int64_t net_connect(int sock, uint32_t ip, uint16_t port)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_CONNECT;
    req.words[0] = (uint64_t) sock;
    req.words[1] = (uint64_t) ip;
    req.words[2] = (uint64_t) port;

    if (!router_forward_timeout(SVC_NET, &req, &rep, 30 * 1000))
        return -1;
    return (int64_t) rep.words[0];
}

int64_t net_listen(int sock, int backlog)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_LISTEN;
    req.words[0] = (uint64_t) sock;
    req.words[1] = (uint64_t) backlog;

    if (!router_forward(SVC_NET, &req, &rep))
        return -1;
    return (int64_t) rep.words[0];
}

int64_t net_accept(int sock)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = NET_ACCEPT;
    req.words[0] = (uint64_t) sock;

    if (!router_forward_timeout(SVC_NET, &req, &rep, 3600 * 1000)
        || (int64_t) rep.words[0] < 0)
        return -1;
    return (int64_t) rep.words[1];
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
