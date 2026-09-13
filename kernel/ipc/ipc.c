/**-----------------------------------------------------------------------------

 @file    ipc.c
 @brief   Implementation of synchronous IPC endpoints

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/ipc.h>
#include <proc/sched.h>

static void endpoint_destroy_object(kernel_object_t *o)
{
    endpoint_t *ep = (endpoint_t *) o;
    kmfree(ep);
}

endpoint_t *endpoint_create(void)
{
    endpoint_t *ep = kmalloc(sizeof(endpoint_t));
    if (ep == NULL)
        return NULL;

    memset(ep, 0, sizeof(endpoint_t));
    spinlock_init(&ep->lock);
    object_init(&ep->obj, OBJ_ENDPOINT, ep, endpoint_destroy_object);
    return ep;
}

kernel_object_t *endpoint_object(endpoint_t *ep)
{
    return &ep->obj;
}

static int ipc_try_recv(endpoint_t *ep, ipc_msg_t *msg)
{
    spinlock_acquire(&ep->lock);

    if (ep->count == 0) {
        spinlock_release(&ep->lock);
        return -1;
    }

    *msg = ep->msgs[ep->head];
    ep->head = (ep->head + 1) % IPC_QUEUE_LEN;
    ep->count--;

    spinlock_release(&ep->lock);
    return 0;
}

int ipc_send(endpoint_t *ep, const ipc_msg_t *msg)
{
    if (ep == NULL || msg == NULL)
        return -1;

    spinlock_acquire(&ep->lock);

    if (ep->count == IPC_QUEUE_LEN) {
        spinlock_release(&ep->lock);
        return -1;              /* queue full */
    }

    ep->msgs[ep->tail] = *msg;
    ep->tail = (ep->tail + 1) % IPC_QUEUE_LEN;
    ep->count++;

    spinlock_release(&ep->lock);

    /* Wake a receiver blocked on this endpoint. A lost wake is handled by the
     * receiver's timeout. */
    sched_wake_key(ep);
    return 0;
}

int ipc_recv(endpoint_t *ep, ipc_msg_t *msg)
{
    if (ep == NULL || msg == NULL)
        return -1;

    for (;;) {
        if (ipc_try_recv(ep, msg) == 0)
            return 0;
        sched_wait_key(ep, 250);
    }
}

int ipc_recv_timeout(endpoint_t *ep, ipc_msg_t *msg, time_t timeout_ms)
{
    if (ep == NULL || msg == NULL)
        return -1;

    if (timeout_ms == 0)
        return ipc_try_recv(ep, msg);

    uint64_t deadline = hpet_get_nanos() + MILLIS_TO_NANOS(timeout_ms);

    for (;;) {
        if (ipc_try_recv(ep, msg) == 0)
            return 0;

        uint64_t now = hpet_get_nanos();
        if (now >= deadline)
            return -1;

        sched_wait_key(ep, (time_t) ((deadline - now) / 1000000ULL) + 1);
    }
}

int ipc_call(endpoint_t *ep, const ipc_msg_t *req, ipc_msg_t *rep)
{
    endpoint_t *reply = endpoint_create();
    if (reply == NULL)
        return -1;

    ipc_msg_t m = *req;
    m.words[IPC_WORDS - 1] = (uint64_t) reply;

    int r = ipc_send(ep, &m);
    if (r == 0)
        r = ipc_recv_timeout(reply, rep, 1000);

    object_unref(endpoint_object(reply));
    return r;
}

int ipc_reply(endpoint_t *ep, const ipc_msg_t *rep)
{
    return ipc_send(ep, rep);
}
