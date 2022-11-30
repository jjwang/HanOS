#include <lib/vector.h>
#include <lib/lock.h>
#include <proc/eventbus.h>
#include <proc/sched.h>

vec_new_static(event_t, eb_publishers);
vec_new_static(event_t, eb_subscribers);

static lock_t eb_lock;

bool eb_publish(task_id_t tid, event_type_t type, event_para_t para)
{
    if (type != EVENT_KEY_PRESSED) {
        return false;
    }

    event_t e = {
        .tid = tid,
        .type = type,
        .para = para};

    lock_lock(&eb_lock);
    vec_push_back(&eb_publishers, e);
    lock_release(&eb_lock);    

    return true;
}

bool eb_subscribe(task_id_t tid, event_type_t type, event_para_t *para)
{
    /* For EVENT_KEY_PRESSED, we need to wait for the next key stroke. */
    if (type != EVENT_KEY_PRESSED) {
        return false;
    }

    event_t e = { 
        .tid = tid,
        .type = type,
        .para = 0};

    lock_lock(&eb_lock);
    vec_push_back(&eb_subscribers, e); 
    lock_release(&eb_lock);  

    e = sched_wait_event(e);
    *para = e.para;

    return true;
}

bool eb_dispatch(void)
{
    lock_lock(&eb_lock);

    while (true) {
        if (vec_length(&eb_publishers) > 0) {
            event_t e = vec_at(&eb_publishers, 0);
            vec_erase(&eb_publishers, 0);
            if (e.type != EVENT_KEY_PRESSED) continue;
            sched_resume_event(e);
            break;
        } else {
            break;
        }
    }   
    lock_release(&eb_lock);

    return true;
}

