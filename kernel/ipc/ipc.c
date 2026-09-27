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

/* Pop one entry. When objs != NULL the caller takes ownership of the staged
 * object references; otherwise they are discarded here. */
static int ipc_try_recv_objs(endpoint_t *ep, ipc_msg_t *msg,
                             kernel_object_t **objs, uint32_t *rights,
                             uint8_t *count)
{
    spinlock_acquire(&ep->lock);

    if (ep->count == 0) {
        spinlock_release(&ep->lock);
        return -1;
    }

    ipc_queue_entry_t *e = &ep->msgs[ep->head];
    *msg = e->msg;

    uint8_t n = e->xfer_count;
    for (uint8_t i = 0; i < n; i++) {
        if (objs != NULL) {
            objs[i] = e->xfer_obj[i];
            if (rights != NULL)
                rights[i] = e->xfer_rights[i];
        } else {
            object_unref(e->xfer_obj[i]);
        }
    }
    if (count != NULL)
        *count = (objs != NULL) ? n : 0;
    e->xfer_count = 0;

    ep->head = (ep->head + 1) % IPC_QUEUE_LEN;
    ep->count--;

    spinlock_release(&ep->lock);
    return 0;
}

int ipc_send_objs(endpoint_t *ep, const ipc_msg_t *msg,
                  kernel_object_t **objs, uint32_t *rights, uint8_t count)
{
    if (ep == NULL || msg == NULL)
        return -1;

    if (count > 2)
        count = 2;

    spinlock_acquire(&ep->lock);

    if (ep->count == IPC_QUEUE_LEN) {
        spinlock_release(&ep->lock);
        return -1;              /* queue full */
    }

    ipc_queue_entry_t *e = &ep->msgs[ep->tail];
    e->msg = *msg;
    e->xfer_count = count;
    for (uint8_t i = 0; i < count; i++) {
        e->xfer_obj[i] = objs[i];
        e->xfer_rights[i] = (rights != NULL) ? rights[i] : 0;
    }

    ep->tail = (ep->tail + 1) % IPC_QUEUE_LEN;
    ep->count++;

    spinlock_release(&ep->lock);

    /* Wake a receiver blocked on this endpoint. A lost wake is handled by the
     * receiver's timeout. */
    sched_wake_key(ep);
    return 0;
}

int ipc_send(endpoint_t *ep, const ipc_msg_t *msg)
{
    return ipc_send_objs(ep, msg, NULL, NULL, 0);
}

int ipc_recv_objs(endpoint_t *ep, ipc_msg_t *msg,
                  kernel_object_t **objs, uint32_t *rights, uint8_t *count)
{
    if (ep == NULL || msg == NULL)
        return -1;

    for (;;) {
        sched_wait_key_begin(ep);
        if (ipc_try_recv_objs(ep, msg, objs, rights, count) == 0) {
            sched_wait_key_cancel();
            return 0;
        }
        sched_wait_key_commit(250);
    }
}

int ipc_recv(endpoint_t *ep, ipc_msg_t *msg)
{
    return ipc_recv_objs(ep, msg, NULL, NULL, NULL);
}

int ipc_recv_timeout_objs(endpoint_t *ep, ipc_msg_t *msg,
                          kernel_object_t **objs, uint32_t *rights,
                          uint8_t *count, time_t timeout_ms)
{
    if (ep == NULL || msg == NULL)
        return -1;

    if (timeout_ms == 0)
        return ipc_try_recv_objs(ep, msg, objs, rights, count);

    uint64_t deadline = hpet_get_nanos() + MILLIS_TO_NANOS(timeout_ms);

    for (;;) {
        sched_wait_key_begin(ep);
        if (ipc_try_recv_objs(ep, msg, objs, rights, count) == 0) {
            sched_wait_key_cancel();
            return 0;
        }

        uint64_t now = hpet_get_nanos();
        if (now >= deadline) {
            sched_wait_key_cancel();
            return -1;
        }

        sched_wait_key_commit((time_t) ((deadline - now) / 1000000ULL) + 1);
    }
}

int ipc_recv_timeout(endpoint_t *ep, ipc_msg_t *msg, time_t timeout_ms)
{
    return ipc_recv_timeout_objs(ep, msg, NULL, NULL, NULL, timeout_ms);
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
