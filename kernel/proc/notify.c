/**-----------------------------------------------------------------------------

 @file    notify.c
 @brief   Implementation of the generic notification primitive

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <ipc/ipc.h>
#include <proc/notify.h>

notify_t notify_system;

void notify_system_init(void)
{
    notify_init(&notify_system);
}

void notify_init(notify_t * n)
{
    n->ep = endpoint_create();
    n->pending = 0;
    spinlock_init(&n->lock);
}

void notify_publish(notify_t * n, uint64_t type, uint64_t para)
{
    if (n == NULL || n->ep == NULL)
        return;

    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = type;
    m.words[0] = para;

    ipc_send(n->ep, &m);
}

bool notify_subscribe(notify_t * n, uint64_t type, uint64_t *para)
{
    if (n == NULL || n->ep == NULL)
        return false;

    /* A single queue is used, so the requested type is not filtered yet. */
    (void) type;

    ipc_msg_t m;
    if (ipc_recv_timeout(n->ep, &m, 1000) != 0)
        return false;

    if (para != NULL)
        *para = m.words[0];

    return true;
}
