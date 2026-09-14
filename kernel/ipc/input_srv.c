/**-----------------------------------------------------------------------------

 @file    input_srv.c
 @brief   Spawn and drive the userspace input server
 @details
 @verbatim

   The input server owns the PS/2 keyboard interrupt and the data/status ports.
   The kernel hands it two endpoints (IRQ notifications in, decoded keys out),
   the I/O-port range and the interrupt line. A small kernel task relays the
   decoded keys onto the event bus so the existing tty path is unchanged.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/input_srv.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <proc/sched.h>
#include <proc/notify.h>
#include <libc/bootinfo.h>

static endpoint_t *input_irq_ep = NULL;
static endpoint_t *input_key_ep = NULL;
static endpoint_t *console_ep = NULL;
static bool input_active = false;
static task_id_t input_spawner = TID_MAX;

/* Run inside sched_execve() before the new task becomes runnable, so the
 * resources are in place before the server can read its bootinfo. */
static void input_spawn_attach(task_t * tc)
{
    if (sched_get_tid() != input_spawner)
        return;

    handle_t h_irq = handle_alloc(&tc->handles, endpoint_object(input_irq_ep),
                                  HANDLE_RIGHT_RECV);
    handle_t h_key = handle_alloc(&tc->handles, endpoint_object(input_key_ep),
                                  HANDLE_RIGHT_SEND);
    handle_t h_con = handle_alloc(&tc->handles, endpoint_object(console_ep),
                                  HANDLE_RIGHT_SEND);

    tc->io_ports[0].first = 0x60;
    tc->io_ports[0].last = 0x64;
    tc->io_port_count = 1;

    if (h_irq == HANDLE_INVALID || h_key == HANDLE_INVALID
        || h_con == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->irq_ep = h_irq;
    bi->key_ep = h_key;
    bi->console_ep = h_con;
    bi->irq_num = 1;
    bi->io_ports[0].first = 0x60;
    bi->io_ports[0].last = 0x64;
    bi->io_port_count = 1;
    tc->bootinfo = bi;
}

_Noreturn static void input_kthread(task_id_t tid)
{
    (void) tid;

    for (;;) {
        ipc_msg_t m;
        if (ipc_recv(input_key_ep, &m) == 0 && m.tag == INPUT_KEY_TAG)
            notify_publish(&notify_system, EVENT_KEY_PRESSED,
                           (event_para_t) m.words[0]);
    }
}

/* Write bytes handed over by a server to the kernel terminal. */
_Noreturn static void console_kthread(task_id_t tid)
{
    (void) tid;

    for (;;) {
        ipc_msg_t m;
        if (ipc_recv(console_ep, &m) == 0 && m.tag == CONSOLE_WRITE_TAG)
            kprintf("%c", (char) m.words[0]);
    }
}

bool input_server_start(void)
{
    input_irq_ep = endpoint_create();
    input_key_ep = endpoint_create();
    console_ep = endpoint_create();
    if (input_irq_ep == NULL || input_key_ep == NULL || console_ep == NULL)
        return false;

    irq_obj_t *io = irq_create(1);
    if (io == NULL)
        return false;
    irq_bind(io, input_irq_ep);

    const char *argv[] = { "input", NULL };

    input_spawner = sched_get_tid();
    sched_set_spawn_hook(input_spawn_attach);
    task_t *tc = sched_execve(DEFAULT_INPUT_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    task_t *tk = sched_new("inkbd", input_kthread, false);
    if (tk == NULL)
        return false;
    sched_add(tk);

    task_t *tcon = sched_new("conkbd", console_kthread, false);
    if (tcon == NULL)
        return false;
    sched_add(tcon);

    input_active = true;
    klogi("input: server started (irq ep 0x%016lx, key ep 0x%016lx)\n",
          input_irq_ep, input_key_ep);
    return true;
}

bool input_server_active(void)
{
    return input_active;
}
