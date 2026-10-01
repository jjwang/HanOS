# 2026-09-13: Cross-CPU Core Task Scheduling

## Overview

- Before this change, the scheduler kept one run queue per core (`tasks_active_table[cpu_id]`).
- `sched_add()` used `smp_get_current_cpu_id()`, so every new task went to the queue of its creating core.
- One core ran the whole workload; the other cores only ran their idle task.

- This change places new tasks on any online core.
- It makes all cross-core scheduler operations safe.

## Design

### 1. Core selection

- `sched_pick_cpu()` walks the SMP information table (`smp_get_info()`) and returns a real APIC id.
- It round-robins over online cores with an atomic counter.
- Using the APIC id instead of the dense core index keeps the run-queue index correct when APIC ids are not contiguous.
- `sched_add()` dispatches to the selected core.
- `sched_execve()` calls `sched_add()`, so user processes spread across cores instead of staying on the caller's core.

### 2. Wake-up model

- Each core runs a periodic APIC timer.
- A task pushed to a remote queue is picked up on that core's next tick.
- No reschedule IPI is required.
- This avoids a new interrupt vector and its lock-order hazards.

### 3. Cross-core lookup and reaping

A parent and its children can now live on different cores. These operations became core-aware:

- `sched_get_task_status()` takes each core's `tasks_lock` in turn.
- It scans every core's running task and run queue. It previously scanned only the local queue.
- `sched_cleanup()` (renamed from `sched_cleanup_local()`) locates and frees a dead task on its owning core.
- `task_idle_proc()` removes a reaped task from its parent's `child_list` by searching all cores (`sched_find_task()`), not just the local queue.

### 4. `child_list` protection

- `task_t` gained a per-task `child_lock`.
- All `child_list` mutations (fork, exec, waitpid, idle reaping) serialize with it.
- The lock is a leaf lock: callers never acquire a run-queue lock while holding it.
- It cannot create an ABBA deadlock with `tasks_lock`.
- `waitpid()` snapshots the child list under the lock and releases it before querying task status.

### 5. Atomic task ids

`task.c` advances `curr_tid` with `__atomic_fetch_add()`, so two cores creating tasks concurrently cannot allocate the same tid.

## Latent bug fixed: delayed-sleeper starvation

- While testing the placement change, a core could livelock.
- Root cause: the run-queue scan in `do_context_switch()` picked a task whose `sched_sleep()` deadline had expired in the same pass that looked for `TASK_READY` tasks.
- `kupdateui` uses `sched_sleep(0)` as a yield, which is immediately wakeable.
- It could be selected before a ready task queued behind it, for example a process dispatched to that core by another core.
- The ready task starved.

The scan runs in two passes: first look for a `TASK_READY` task; then fall back to a task whose sleep deadline expired when no task is ready.

## Follow-up fix: cross-core `waitpid` could block forever

After the placement change, running a command in the shell (`fork` + `execve` with the new process dispatched to another core) could leave the shell stuck in `waitpid` forever. Two problems combined:

1. `sched_exit()` set the task to `TASK_DYING`, then released `child_lock`, which re-enabled interrupts. A timer tick landing in that window switched the task out. `TASK_DYING` is never selected again, so the task stayed `DYING` forever. Its parent kept seeing a live child.
2. `sched_get_task_status()` mapped a `TASK_DYING` task to `TASK_UNKNOWN`, so a parent whose child was stuck in `DYING` could never observe it as finished.

Fixes:

- `sched_exit()` disables interrupts before the `DYING -> DEAD` transition and keeps them disabled until the final context switch, so the transition is atomic with respect to the timer.
- `sched_get_task_status()` reports a `TASK_DYING` task that has no live children as `TASK_DEAD`. `waitpid()` can then reap it through the cross-core `sched_cleanup()` even when the owning core's idle task is starved.

## Follow-up fix: console output throttle used a per-CPU clock

- `k_write()` rate-limited a task's first write when the previous writer was a different task.
- It waited until `sched_get_ticks()` advanced by 250.
- `sched_get_ticks()` returns `tasks_coordinate[cpu_id]`, a **per-CPU** counter.
- Once tasks run on different cores, a counter recorded on one core is compared against another core's counter.
- The wait can last many seconds.
- Running `ls` (dispatched to core A) and returning to the shell (on core B) made the shell spend ~10 seconds before printing its next prompt.

Fix: use the global HPET clock (`hpet_get_nanos()`) with a 250 ms threshold. The throttle behaves identically on every core.

## Files Changed

| File | Changes |
|------|---------|
| `kernel/proc/sched.c` | `sched_pick_cpu()`, `sched_find_task()`, cross-core `sched_add()`/`sched_execve()`, cross-core `sched_get_task_status()`, `sched_cleanup()`, global `task_idle_proc()` parent lookup, `child_lock` usage, locked `sched_resume_event()`, two-pass ready/sleep selection in `do_context_switch()` |
| `kernel/proc/sched.h` | `sched_cleanup_local()` → `sched_cleanup()` |
| `kernel/proc/task.c` | Atomic `curr_tid` allocation, `child_lock` reset on fork, locked `child_list` push |
| `kernel/proc/task.h` | Added `lock_t child_lock` to `task_t` |
| `kernel/proc/syscall.c` | `waitpid()` snapshots `child_list` under `child_lock` and uses cross-core `sched_cleanup()`; added `base/kmalloc.h` include; `k_write()` output throttle now uses the global HPET clock instead of the per-CPU tick counter |
| `kernel/kconfig.h` | Keep `ENABLE_KLOG_DEBUG` disabled |

## Testing

Booted under QEMU/KVM with `-smp 2` and `-smp 4` over dozens of runs. The scheduling log shows tasks, their children, and their children's children placed on different cores, e.g.:

```
SCHED: CPU 0 dispatches tid 2 to CPU 0
SCHED: CPU 0 dispatches tid 3 to CPU 1
SCHED: child tid 5 and parent tid 3
SCHED: CPU 1 dispatches tid 5 to CPU 0
SCHED: CPU 0 dispatches tid 7 to CPU 1
```

`fork` + `execve` + `exit` across core boundaries (running `help`/`ls` in the shell) completes without page faults, GPFs, or deadlocks.
