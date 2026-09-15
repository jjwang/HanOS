/**-----------------------------------------------------------------------------

 @file    selftest.c
 @brief   Boot-time self-test for the IPC/handle layer
 @details
 @verbatim

   Runs in a kernel process once the scheduler is up. It exercises the endpoint
   queue, the IPC timeout path, and the per-process handle table, then logs
   "MK: selftest PASS" or "MK: selftest FAIL".

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/klog.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <ipc/selftest.h>
#include <proc/sched.h>
#include <proc/process.h>

_Noreturn void mk_selftest_task(pid_t pid)
{
    (void) pid;

    bool ok = true;
    endpoint_t *ep = endpoint_create();

    if (ep == NULL) {
        ok = false;
    } else {
        ipc_msg_t m;
        memset(&m, 0, sizeof(m));
        m.tag = 0x1234;
        m.words[0] = 0xABCDEF;

        if (ipc_send(ep, &m) != 0)
            ok = false;

        ipc_msg_t r;
        if (ipc_recv_timeout(ep, &r, 100) != 0 || r.tag != 0x1234
            || r.words[0] != 0xABCDEF)
            ok = false;

        /* An empty endpoint must time out instead of blocking forever. */
        ipc_msg_t r2;
        if (ipc_recv_timeout(ep, &r2, 10) == 0)
            ok = false;

        /* Handle table round trip. */
        process_t *t = sched_get_current_process();
        handle_t h = handle_alloc(&t->handles, endpoint_object(ep),
                                  HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV);
        if (h == HANDLE_INVALID) {
            ok = false;
        } else {
            kernel_object_t *o = handle_get(&t->handles, h, HANDLE_RIGHT_SEND);
            if (o == NULL || o->type != OBJ_ENDPOINT)
                ok = false;
            if (handle_close(&t->handles, h) != 0)
                ok = false;
        }

        object_unref(endpoint_object(ep));
    }

    /* IRQ object delivery to a bound endpoint. Line 20 is outside the
     * hardware range so this cannot interfere with a real driver. */
    if (ok) {
        irq_obj_t *io = irq_create(20);
        endpoint_t *iep = endpoint_create();
        if (io == NULL || iep == NULL) {
            ok = false;
        } else {
            irq_bind(io, iep);
            if (!irq_deliver(20))
                ok = false;

            ipc_msg_t im;
            if (ipc_recv_timeout(iep, &im, 100) != 0
                || im.tag != IRQ_NOTIFY_TAG || im.words[0] != 20)
                ok = false;

            object_unref(endpoint_object(iep));
            object_unref(irq_object(io));
        }
    }

    klogi("MK: selftest %s\n", ok ? "PASS" : "FAIL");

    for (;;)
        asm volatile ("hlt");
}
