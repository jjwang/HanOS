/**-----------------------------------------------------------------------------

 @file    eventbus.c
 @brief   Implementation of the event bus system
 @details
 @verbatim

  This file contains the implementation of the event bus system used in the HanOS
  kernel. The event bus allows tasks to publish and subscribe to events, enabling
  inter-task communication and event-driven execution. The functions provided
  facilitate publishing events, subscribing to events, and dispatching events to
  their respective handlers. The event bus is designed to handle specific types of
  events, such as key press events.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <sys/hpet.h>
#include <base/vector.h>
#include <base/spinlock.h>
#include <proc/eventbus.h>
#include <proc/sched.h>

vec_new_static(event_t, eb_publishers);
vec_new_static(event_t, eb_subscribers);

static lock_t eb_lock;
static bool eb_debug = false;

bool eb_publish(task_id_t tid, event_type_t type, event_para_t para)
{
    if (type != EVENT_KEY_PRESSED) {
        return false;
    }

    event_t e = {
        .pub_tid = tid,
        .sub_tid = TID_MAX,
        .type = type,
        .para = para
    };

    e.timestamp = hpet_get_nanos();

    lock_lock(&eb_lock);
    vec_push_back(&eb_publishers, e);
    lock_release(&eb_lock);

    if (eb_debug) {
        klogi("EB: task id %ld published  para 0x%8x with type 0x%8x "
              "and millis %ld, ticks %ld\n",
              tid, para, type, hpet_get_millis(), sched_get_ticks());
    }

    return true;
}

bool eb_subscribe(task_id_t tid, event_type_t type, event_para_t * para)
{
    /* For EVENT_KEY_PRESSED, we need to wait for the next key stroke. */
    if (type != EVENT_KEY_PRESSED) {
        return false;
    }

    event_t e = {
        .pub_tid = TID_MAX,
        .sub_tid = tid,
        .type = type,
        .para = 0
    };

    e.timestamp = hpet_get_nanos();

    lock_lock(&eb_lock);
    vec_push_back(&eb_subscribers, e);
    lock_release(&eb_lock);

    e = sched_wait_event(e);
    *para = e.para;

    if (eb_debug) {
        klogi("EB: task id %ld subscribed para 0x%8x with type 0x%8x "
              "and millis %ld, ticks %ld\n",
              tid, e.para, type, hpet_get_millis(), sched_get_ticks());
    }

    return true;
}

bool eb_dispatch(void)
{
    if (!lock_try(&eb_lock)) {
        return false;
    }

    while (true) {
        if (vec_length(&eb_publishers) > 0) {
            event_t e = vec_at(&eb_publishers, 0);
            if (e.type != EVENT_KEY_PRESSED) {
                vec_erase(&eb_publishers, 0);
                continue;
            }
            if (sched_resume_event(e)) {
                vec_erase(&eb_publishers, 0);
            }
            break;
        } else {
            break;
        }
    }
    lock_release(&eb_lock);

    return true;
}
