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
#include <stdint.h>

#include <proc/process.h>
#include <lib/time.h>

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
/* IPC receive waits are split so a message queued between the queue check and
 * the sleep cannot be lost: arm before checking, cancel on success, commit to
 * block. sched_wake_key() delivers to the armed process. */
void sched_wait_key_begin(void *key);
void sched_wait_key_commit(time_t ms);
void sched_wait_key_commit_infinite(void);
void sched_wait_key_cancel(void);
void sched_wake_key(void *key);
int64_t sched_wake_key_n(void *key, int64_t n);
void sched_kill_group(pid_t tgid, pid_t except);
/* Make a sleeping process runnable, on whichever core holds it. Used to wake
 * a process when a signal is queued on it. */
void sched_wake_process(process_t * t);
void sched_mark_fds_ready(pid_t pid);
void sched_wait_fds_ready(process_t *t);
process_t *sched_get_current_process(void);
uint16_t sched_get_cpu_num(void);
uint64_t sched_get_ticks(void);
pid_t sched_get_pid(void);

process_t *sched_execve(const char *path, const char *argv[],
                     const char *envp[], const char *cwd);
