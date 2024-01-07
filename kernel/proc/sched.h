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
#include <base/time.h>

_Noreturn void task_idle_proc(task_id_t tid);

void sched_debug(bool showlog);

void sched_init(const char *name, uint16_t cpu_id);
task_t *sched_new(const char *name, void (*entry)(task_id_t), bool usermode);
void sched_add(task_t *t);
void sched_sleep(time_t ms);
task_id_t sched_fork(void);
void sched_exit(int64_t status);
event_t sched_wait_event(event_t event);
bool sched_resume_event(event_t event);
task_t *sched_get_current_task(void);
uint16_t sched_get_cpu_num(void);
uint64_t sched_get_ticks(void);
task_id_t sched_get_tid(void);
task_status_t sched_get_task_status(task_id_t tid);

task_t *sched_execve(
    const char *path, const char *argv[], const char *envp[], const char *cwd);
