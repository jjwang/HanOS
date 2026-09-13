/**-----------------------------------------------------------------------------

 @file    eventbus.c
 @brief   Implementation of the event bus system
 @details
 @verbatim

   The event bus is now a thin wrapper over the generic notification primitive
   (proc/notify.h), which is itself built on IPC endpoints. A publisher (e.g.
   the keyboard ISR) sends a typed notification; a subscriber (e.g. the tty
   read path) blocks until one arrives.

   eb_dispatch() is kept for source compatibility with the scheduler but no
   longer needs to poll: waking is done directly by notify_publish().

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>

#include <base/klog.h>
#include <proc/eventbus.h>
#include <proc/notify.h>

static notify_t eb_notify;
static bool eb_ready = false;

void eb_init(void)
{
    notify_init(&eb_notify);
    eb_ready = true;
}

bool eb_publish(task_id_t tid, event_type_t type, event_para_t para)
{
    (void) tid;

    if (!eb_ready)
        return false;

    notify_publish(&eb_notify, (uint64_t) type, (uint64_t) para);
    return true;
}

bool eb_subscribe(task_id_t tid, event_type_t type, event_para_t * para)
{
    (void) tid;

    if (!eb_ready)
        return false;

    uint64_t p = 0;
    if (!notify_subscribe(&eb_notify, (uint64_t) type, &p))
        return false;

    if (para != NULL)
        *para = p;

    return true;
}

bool eb_dispatch(void)
{
    return true;
}
