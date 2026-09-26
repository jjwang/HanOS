/**-----------------------------------------------------------------------------

 @file    block_srv.c
 @brief   Spawn the userspace block server

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>
#include <libc/bootinfo.h>
#include <libc/protocol.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/block_srv.h>
#include <ipc/ipc.h>
#include <mm/memobj.h>
#include <proc/sched.h>

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

    /* ATA PIO: command block 0x1F0..0x1F7, control 0x3F6..0x3F7. */
    tc->io_ports[0].first = 0x1F0;
    tc->io_ports[0].last = 0x1F7;
    tc->io_ports[1].first = 0x3F6;
    tc->io_ports[1].last = 0x3F7;
    tc->io_port_count = 2;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;
    bi->io_ports[0].first = 0x1F0;
    bi->io_ports[0].last = 0x1F7;
    bi->io_ports[1].first = 0x3F6;
    bi->io_ports[1].last = 0x3F7;
    bi->io_port_count = 2;
    tc->bootinfo = bi;

    klogi("block: attached ATA ports to pid %ld (ep handle %ld)\n",
          (long)tc->pid, (long)h);
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
            klogw("block: tag 0x%lx timed out\n", (unsigned long)tag);
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
                  memcmp(r, "HANOS-BLOCK", 11) ? "OK" : "FAIL");
            memobj_unref(ro);
        }
    }
}
