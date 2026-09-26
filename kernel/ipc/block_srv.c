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

/* Send BLOCK_GET_INFO, moving a reply endpoint to the server, and log the
 * reply. Runs in a kernel task (low-level IPC, no user buffer). */
void block_server_probe(void)
{
    endpoint_t *ep = block_in_ep;
    endpoint_t *reply = endpoint_create();

    if (ep == NULL || reply == NULL)
        return;

    handle_t dummy;

    (void) dummy;

    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = BLOCK_GET_INFO;

    /* Keep our own reference while the server also receives one. */
    object_ref(endpoint_object(reply));
    kernel_object_t *objs[1] = { endpoint_object(reply) };
    uint32_t rights[1] = { HANDLE_RIGHT_SEND };

    if (ipc_send_objs(ep, &m, objs, rights, 1) != 0) {
        object_unref(endpoint_object(reply));
        object_unref(endpoint_object(reply));
        return;
    }

    ipc_msg_t rep;
    if (ipc_recv_timeout(reply, &rep, 2000) == 0) {
        klogi("block: GET_INFO sector_size %lu count %lu\n",
              rep.words[0], rep.words[1]);
    } else {
        klogw("block: GET_INFO timed out\n");
    }

    object_unref(endpoint_object(reply));
}
