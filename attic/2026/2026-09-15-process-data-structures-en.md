# 2026-09-15: Process Data Structures

## Overview

Rename the process (previously "task") data structures and give them clearer names and types. Replace the per-CPU linear pid lookup with a global process table.

## Changes

- Rename `task_t` to `process_t` and `task_id_t` to `pid_t`. Retire the old int32 `pid_t` in `fs/vfs.h` to `posix_pid_t`; only the unused `siginfo_t` uses it. `pid_t` now means process id.
- Rename `task_regs_t`, `task_status_t`, `task_mode_t`, `task_priority_t` and the `TASK_*` constants to `process_*` and `PROC_*`.
- Rename `kernel/proc/task.{c,h}` to `kernel/proc/process.{c,h}`.
- Rename the id fields `tid`/`ptid` to `pid`/`ppid`. Drop the unused `tstack_limit` and `aux` fields and the dead `event_t` fields. Group the struct into context, identity, resources, address space and signals.
- Add `io_port_range_t` (shared with `bootinfo_t`) and `signal_state_t`.
- Add a global process table.
- `process.c` stores an intrusive chained hash keyed by pid, exposed as `process_lookup()`.
- `sched_find_task()` and `sched_get_task_status()` use the table instead of scanning every run queue.
- Rename the scheduler globals to `run_queues`, `running_task`, `idle_task`, `tick_count` and `run_queue_lock`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/process.{c,h}` | process struct, types and global process table |
| `kernel/proc/task.{c,h}` | renamed to `process.{c,h}` |
| `kernel/proc/sched.{c,h}` | use `process_t`/`pid_t`, renamed scheduler state |
| `libc/bootinfo.h` | shared `io_port_range_t` |
| kernel-wide | `task`/`tid` naming replaced by `process`/`pid` |

## Testing

- `make -C kernel clean && make -C kernel` builds with no warnings and the full image builds.
- SMP2 and SMP4 boots reach `name "hansh"` with no panic. The shell still runs `ls`/`pwd`. The kernel reaps dead processes.
