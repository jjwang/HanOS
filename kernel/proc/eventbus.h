#pragma once

/* Event related data structures are defined in task.h */
#include <proc/task.h>

bool eb_publish(task_id_t tid, event_type_t type, event_para_t para);
bool eb_subscribe(task_id_t tid, event_type_t type, event_para_t *para);
bool eb_dispatch(void);

