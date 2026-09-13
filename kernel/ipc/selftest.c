/**-----------------------------------------------------------------------------

 @file    selftest.c
 @brief   Boot-time self-test for the IPC/handle layer
 @details
 @verbatim

   Runs in a kernel task once the scheduler is up. It exercises the endpoint
   queue, the IPC timeout path, and the per-task handle table, then logs
   "MK: selftest PASS" or "MK: selftest FAIL".

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/klog.h>
#include <ipc/ipc.h>
#include <ipc/selftest.h>
#include <proc/sched.h>
#include <proc/task.h>

_Noreturn void mk_selftest_task(task_id_t tid)
{
    (void) tid;

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
        task_t *t = sched_get_current_task();
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

    klogi("MK: selftest %s\n", ok ? "PASS" : "FAIL");

    for (;;)
        asm volatile ("hlt");
}
