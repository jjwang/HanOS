/**-----------------------------------------------------------------------------

 @file    service.c
 @brief   Implementation of the kernel-side service router

 **-----------------------------------------------------------------------------
 */
#include <base/spinlock.h>
#include <service/service.h>

static struct {
    endpoint_t *ep;
    task_id_t owner;
} services[SVC_COUNT];

static spinlock_t service_lock;

void service_register(service_id_t id, endpoint_t *ep, task_id_t owner)
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
    if (ep == NULL)
        return false;

    return ipc_call(ep, req, rep) == 0;
}
