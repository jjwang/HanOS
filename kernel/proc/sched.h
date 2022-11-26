/**-----------------------------------------------------------------------------

 @file    sched.h
 @brief   Definition of scheduling related functions
 @details
 @verbatim

  Include Context Switching, Scheduling Algorithms etc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/task.h>
#include <lib/time.h>

void task_debug(void);

void sched_init(uint16_t cpu_id);
task_t *sched_add(void (*entry)(task_id_t), bool usermode);
void sched_sleep(time_t ms);
event_t sched_wait_event(event_t event);
void sched_resume_event(event_t event);
task_t *sched_get_current_task();
uint16_t sched_get_cpu_num();
uint64_t sched_get_ticks();
task_id_t sched_get_tid();

