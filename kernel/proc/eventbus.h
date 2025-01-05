/**-----------------------------------------------------------------------------

 @file    eventbus.h
 @brief   Definitions for the event bus system
 @details
 @verbatim

  This file contains the definitions for the event bus system used in the HanOS
  kernel. The event bus allows tasks to publish and subscribe to events, enabling
  inter-task communication and event-driven execution. The functions provided
  facilitate publishing events, subscribing to events, and dispatching events to
  their respective handlers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

/* Event related data structures are defined in task.h */
#include <proc/task.h>

bool eb_publish(task_id_t tid, event_type_t type, event_para_t para);
bool eb_subscribe(task_id_t tid, event_type_t type, event_para_t *para);
bool eb_dispatch(void);

