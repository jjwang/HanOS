/**-----------------------------------------------------------------------------

 @file    input_srv.c
 @brief   Spawn and drive the userspace input server
 @details
 @verbatim

   The input server owns the PS/2 keyboard and mouse interrupts with their
   data/status ports, plus COM1 for a serial console. The kernel hands it the
   endpoints (IRQ notifications in, decoded keys out), the I/O-port ranges and
   the interrupt lines. A small kernel process relays the decoded keys to the
   tty server.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/input_srv.h>
#include <ipc/ipc.h>
#include <srv/tty_srv.h>
#include <ipc/irq.h>
#include <proc/sched.h>
#include <device/display/gfx.h>
#include <arch/x64/idt.h>
#include <bootinfo.h>

static endpoint_t *input_irq_ep = NULL;
static endpoint_t *input_key_ep = NULL;
static bool input_active = false;
static pid_t input_spawner = PID_MAX;

/* Run inside sched_execve() before the new process becomes runnable, so the
 * resources are in place before the server can read its bootinfo. */
static void input_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != input_spawner)
        return;

    handle_t h_irq = handle_alloc(&tc->handles, endpoint_object(input_irq_ep),
                                  HANDLE_RIGHT_RECV);
    handle_t h_key = handle_alloc(&tc->handles, endpoint_object(input_key_ep),
                                  HANDLE_RIGHT_SEND);

    /* PS/2 data/status ports, then COM1 for the serial console. */
    tc->io_ports[0].first = 0x60;
    tc->io_ports[0].last = 0x64;
    tc->io_ports[1].first = 0x3F8;
    tc->io_ports[1].last = 0x3FF;
    tc->io_port_count = 2;

    if (h_irq == HANDLE_INVALID || h_key == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->irq_ep = h_irq;
    bi->key_ep = h_key;
    bi->irq_num = 1;
    bi->io_ports[0].first = 0x60;
    bi->io_ports[0].last = 0x64;
    bi->io_ports[1].first = 0x3F8;
    bi->io_ports[1].last = 0x3FF;
    bi->io_port_count = 2;
    tc->bootinfo = bi;
}

_Noreturn static void input_kthread(pid_t pid)
{
    (void) pid;

    for (;;) {
        ipc_msg_t m;
        if (ipc_recv(input_key_ep, &m) != 0)
            continue;

        if (m.tag == INPUT_MOUSE_TAG) {
            gfx_cursor_move((int32_t) (int64_t) m.words[0],
                            (int32_t) (int64_t) m.words[1]);
        } else if (m.tag == INPUT_KEY_TAG) {
            /* The tty server owns /dev/tty. */
            tty_server_deliver_key((uint8_t) m.words[0]);
        }
    }
}

bool input_server_start(void)
{
    input_irq_ep = endpoint_create();
    input_key_ep = endpoint_create();
    if (input_irq_ep == NULL || input_key_ep == NULL)
        return false;

    irq_obj_t *io = irq_create(1);
    irq_obj_t *io12 = irq_create(12);
    irq_obj_t *io4 = irq_create(4);
    if (io == NULL || io12 == NULL || io4 == NULL)
        return false;
    irq_bind(io, input_irq_ep);
    irq_bind(io12, input_irq_ep);
    irq_bind(io4, input_irq_ep);

    /* Unmask the keyboard, mouse and serial lines on the PIC (IRQ2 cascades
     * the slave PIC that carries IRQ12). */
    irq_clear_mask(1);
    irq_clear_mask(2);
    irq_clear_mask(4);
    irq_clear_mask(12);

    const char *argv[] = { "input", NULL };

    input_spawner = sched_get_pid();
    sched_set_spawn_hook(input_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_INPUT_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    process_t *tk = sched_new("inkbd", input_kthread, false);
    if (tk == NULL)
        return false;
    sched_add(tk);

    input_active = true;
    klogi("input: server started (irq ep 0x%016lx, key ep 0x%016lx)\n",
          input_irq_ep, input_key_ep);
    return true;
}

bool input_server_active(void)
{
    return input_active;
}
