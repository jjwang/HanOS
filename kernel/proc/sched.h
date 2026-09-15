/**-----------------------------------------------------------------------------

 @file    sched.h
 @brief   Definition of scheduling related functions
 @details
 @verbatim

  This header file provides the definitions for scheduling-related functions 
  used in the HanOS kernel. It includes declarations for context switching, 
  scheduling algorithms, process management, and various scheduler utilities.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/process.h>
#include <base/time.h>

#define SCHED_SWITCH_TIME_CYCLE         0
#define SCHED_SWITCH_SLEEP              1
#define SCHED_SWITCH_FORK               2

_Noreturn void process_idle(pid_t pid);

void sched_debug(bool showlog);

void sched_init(const char *name, uint16_t cpu_id);
void sched_set_spawn_hook(void (*hook) (process_t *));
process_t *sched_new(const char *name, void (*entry)(pid_t),
                  bool usermode);
void sched_add(process_t *t);
void sched_sleep_impl(time_t ms, bool advanced);
#define sched_sleep(x)  sched_sleep_impl(x, false)
pid_t sched_fork(void);
void sched_exit(int64_t status);
void sched_wait_child(time_t ms);
void sched_wait_key(void *key, time_t ms);
void sched_wake_key(void *key);
process_t *sched_get_current_process(void);
uint16_t sched_get_cpu_num(void);
uint64_t sched_get_ticks(void);
pid_t sched_get_pid(void);
process_status_t sched_get_process_status(pid_t pid);
int sched_reap(pid_t pid, int64_t *status);

process_t *sched_execve(const char *path, const char *argv[],
                     const char *envp[], const char *cwd);
