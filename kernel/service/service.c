/**-----------------------------------------------------------------------------

 @file    service.c
 @brief   Implementation of the kernel-side service router

 **-----------------------------------------------------------------------------
 */
#include <base/spinlock.h>
#include <service/service.h>
#include <base/time.h>
#include <ipc/object.h>
#include <proc/sched.h>

static struct {
    endpoint_t *ep;
    pid_t owner;
} services[SVC_COUNT];

static spinlock_t service_lock;

void service_register(service_id_t id, endpoint_t *ep, pid_t owner)
{
    if (id >= SVC_COUNT)
        return;

    spinlock_acquire(&service_lock);
    services[id].ep = ep;
    services[id].owner = owner;
    spinlock_release(&service_lock);
}

endpoint_t *service_lookup(service_id_t id)
{
    if (id >= SVC_COUNT)
        return NULL;

    return services[id].ep;
}

bool service_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep)
{
    endpoint_t *ep = service_lookup(id);
    process_t *cur = sched_get_current_process();

    if (ep == NULL || cur == NULL)
        return false;

    /* The callee may be a userspace server, which can only reply through a
     * handle: create a reply endpoint and move it to the receiver in xfer[0]
     * (the server replies on it and closes it). */
    endpoint_t *reply = endpoint_create();
    if (reply == NULL)
        return false;

    object_ref(endpoint_object(reply)); /* reference moved to the server */

    kernel_object_t *objs[3];
    uint32_t rights[3];
    uint8_t n = 0;
    objs[n] = endpoint_object(reply);   /* xfer[0] */
    rights[n++] = HANDLE_RIGHT_SEND;

    /* Move the request's own handles (a path memory object, ...) after the
     * reply endpoint, so the server receives them in xfer[1..]. */
    uint8_t xn = req->xfer_count > 2 ? 2 : req->xfer_count;
    for (uint8_t i = 0; i < xn; i++) {
        kernel_object_t *o =
            handle_get(&cur->handles, req->xfer[i], HANDLE_RIGHT_TRANSFER);

        if (o == NULL) {
            for (uint8_t k = 0; k < n; k++)
                object_unref(objs[k]);
            object_unref(endpoint_object(reply));
            return false;
        }
        object_ref(o);
        handle_close(&cur->handles, req->xfer[i]);
        objs[n] = o;
        rights[n] = (o->type == OBJ_MEMORY)
            ? (HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_MAP)
            : (HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV);
        n++;
    }

    ipc_msg_t m = *req;
    m.xfer_count = 0;

    if (ipc_send_objs(ep, &m, objs, rights, n) != 0) {
        for (uint8_t k = 0; k < n; k++)
            object_unref(objs[k]);
        object_unref(endpoint_object(reply));
        return false;
    }

    int r = ipc_recv_timeout(reply, rep, 1000);
    object_unref(endpoint_object(reply));
    return r == 0;
}
