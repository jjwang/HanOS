/**-----------------------------------------------------------------------------

 @file    block_srv.c
 @brief   Spawn the userspace block server
 @details
 @verbatim

  Creates the service endpoint, grants the AHCI controller's ABAR and a
  contiguous DMA region, and spawns /bin/block; the probe does a boot-time
  BLOCK_GET_INFO/READ/WRITE round trip.

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
#include <mm/mm.h>
#include <mm/memobj.h>
#include <ipc/ipc.h>
#include <proc/sched.h>
#include <srv/block_srv.h>
#include <arch/x64/pci.h>

/* Where the AHCI ABAR and the DMA region map in the block server. */
#define BLOCK_MMIO_VADDR    0x30000000UL
#define BLOCK_DMA_VADDR     0x40000000UL
#define BLOCK_DMA_SIZE      (256 * 1024)

static endpoint_t *block_in_ep = NULL;
static pid_t block_spawner = PID_MAX;
static bool block_active = false;

static void block_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != block_spawner)
        return;

    if (block_in_ep == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(block_in_ep),
                              HANDLE_RIGHT_RECV);
    if (h == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;

    /* Grant the AHCI controller's ABAR (BAR5). */
    pci_device_t dev;

    if (pci_find_class(PCI_CLASS_STORAGE, 0x06, &dev)) {
        uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);

        /* Bus master + memory space, then map ABAR into the server. */
        pci_outw(id, PCI_CONFIG_COMMAND, pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

        pci_bar_t bar;

        pci_get_bar(&bar, id, 5);
        if (bar.u.address != NULL && bar.size != 0) {
            vmm_map(tc->addrspace, BLOCK_MMIO_VADDR, (uint64_t) bar.u.address,
                    NUM_PAGES(bar.size), VMM_FLAGS_MMIO | VMM_FLAG_USER);
            bi->block_mmio_vaddr = BLOCK_MMIO_VADDR;
            bi->block_mmio_size = bar.size;
            klogi("block: granted AHCI %04x:%04x ABAR 0x%lx at 0x%lx\n",
                  dev.vendor_id, dev.device_id, (uint64_t) bar.u.address,
                  (uint64_t) BLOCK_MMIO_VADDR);
        } else {
            klogw("block: AHCI ABAR missing\n");
        }
    } else {
        klogw("block: no AHCI controller\n");
    }

    /* Contiguous DMA region for the command list, FIS, tables and data. */
    void *dma = kmalloc_chunk(BLOCK_DMA_SIZE, __func__, __LINE__);

    if (dma != NULL) {
        uint64_t dma_phys = VIRT_TO_PHYS((uint64_t) dma);

        vmm_map(tc->addrspace, BLOCK_DMA_VADDR, dma_phys,
                NUM_PAGES(BLOCK_DMA_SIZE), VMM_FLAGS_DEFAULT | VMM_FLAG_USER);
        bi->block_dma_vaddr = BLOCK_DMA_VADDR;
        bi->block_dma_phys = dma_phys;
        bi->block_dma_size = BLOCK_DMA_SIZE;
        klogi("block: granted DMA 0x%lx (%ld bytes) at 0x%lx\n", dma_phys,
              (int64_t) BLOCK_DMA_SIZE, (int64_t) BLOCK_DMA_VADDR);
    }

    tc->bootinfo = bi;

    klogi("block: attached AHCI to pid %ld (ep handle %ld)\n",
          (int64_t)tc->pid, (int64_t)h);
}

bool block_server_start(void)
{
    block_in_ep = endpoint_create();
    if (block_in_ep == NULL)
        return false;

    const char *argv[] = { "block", NULL };

    block_spawner = sched_get_pid();
    sched_set_spawn_hook(block_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_BLOCK_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    block_active = true;
    klogi("block: server started (ep 0x%016lx)\n", (uint64_t) block_in_ep);
    return true;
}

bool block_server_active(void)
{
    return block_active;
}

endpoint_t *block_server_endpoint(void)
{
    return block_in_ep;
}

/* Send one request, moving a reply endpoint (and optionally a memory object)
 * to the server, then log the reply. Runs in a kernel task (low-level IPC; a
 * kernel process has no user buffer to go through k_ipc_send). */
static void block_probe_rpc(endpoint_t *ep, uint32_t tag, uint64_t lba,
                            uint64_t count, memobj_t *mo)
{
    endpoint_t *reply = endpoint_create();
    if (reply == NULL)
        return;

    object_ref(endpoint_object(reply)); /* the reference moved to the server */

    kernel_object_t *objs[2];
    uint32_t rights[2];
    uint8_t n = 0;
    objs[n] = endpoint_object(reply);
    rights[n++] = HANDLE_RIGHT_SEND;

    if (mo != NULL) {
        object_ref(memobj_object(mo));
        objs[n] = memobj_object(mo);
        rights[n++] = HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
            | HANDLE_RIGHT_MAP;
    }

    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = tag;
    m.words[0] = lba;
    m.words[1] = count;

    if (ipc_send_objs(ep, &m, objs, rights, n) != 0) {
        /* Nothing was queued: drop the references we took to move. */
        object_unref(endpoint_object(reply));
        if (mo != NULL)
            object_unref(memobj_object(mo));
    } else {
        ipc_msg_t rep;
        if (ipc_recv_timeout(reply, &rep, 3000) != 0) {
            klogw("block: tag 0x%lx timed out\n", (uint64_t)tag);
        } else if (tag == BLOCK_GET_INFO) {
            klogi("block: GET_INFO sector_size %lu count %lu\n",
                  rep.words[0], rep.words[1]);
        } else if (tag == BLOCK_READ && mo != NULL) {
            uint8_t *b = (uint8_t *) PHYS_TO_VIRT(memobj_page(mo, 0));
            klogi("block: READ lba %lu status %ld sig %02x%02x\n", lba,
                  (int64_t) rep.words[0], b[510], b[511]);
        } else if (tag == BLOCK_WRITE) {
            klogi("block: WRITE lba %lu status %ld\n", lba,
                  (int64_t) rep.words[0]);
        }
    }

    object_unref(endpoint_object(reply));       /* our own ref */
}

void block_server_probe(void)
{
    endpoint_t *ep = block_in_ep;
    if (ep == NULL)
        return;

    block_probe_rpc(ep, BLOCK_GET_INFO, 0, 0, NULL);

    /* Read sector 0 (the MBR) through a memory object and check its signature. */
    memobj_t *mo = memobj_create(512);
    if (mo != NULL) {
        block_probe_rpc(ep, BLOCK_READ, 0, 1, mo);
        memobj_unref(mo);
    }

    /* Write a pattern to a scratch sector and read it back. */
    memobj_t *wo = memobj_create(512);
    if (wo != NULL) {
        uint8_t *w = (uint8_t *) PHYS_TO_VIRT(memobj_page(wo, 0));

        memset(w, 0, 512);
        memcpy(w, "HANOS-BLOCK", 11);
        block_probe_rpc(ep, BLOCK_WRITE, 8, 1, wo);
        memobj_unref(wo);

        memobj_t *ro = memobj_create(512);
        if (ro != NULL) {
            block_probe_rpc(ep, BLOCK_READ, 8, 1, ro);
            uint8_t *r = (uint8_t *) PHYS_TO_VIRT(memobj_page(ro, 0));
            klogi("block: WRITE/READ lba8 %s\n",
                  memcmp(r, "HANOS-BLOCK", 11) == 0 ? "OK" : "FAIL");
            memobj_unref(ro);
        }
    }
}
