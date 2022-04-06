/**-----------------------------------------------------------------------------

 @file    sched.h
 @brief   Definition of scheduling related functions
 @details
 @verbatim

  Include Context Switching, Scheduling Algorithms etc.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/task.h>
#include <lib/time.h>

void sched_init(uint16_t cpu_id);
void sched_add(void (*entry)(task_id_t));
void sched_sleep(time_t ms);
task_t* sched_get_current_task();
