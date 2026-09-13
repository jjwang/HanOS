# 2026-09-13: Blocking `waitpid` with Exit Status and Child-Exit Wakeup

## Overview

`k_waitpid()` in `kernel/proc/syscall.c` was rewritten. The previous version had
two serious bugs and no exit-status support.

## Bugs fixed

1. **`pid > 0` busy-spun forever.** Once the child was already dead the branch
   fell through and looped without sleeping, and the code after it was
   unreachable.
2. **`pid` was compared as 64-bit.** The syscall passes a 32-bit
   zero-extended `-1`, i.e. `0x00000000FFFFFFFF` (`4294967295`), so
   `pid == -1` never matched. `init`'s `wait(-1)` therefore returned `ECHILD`
   immediately and respawned the shell in a loop.

## New behaviour

- `pid == -1` / `pid == 0` waits for any child; `pid > 0` waits for that child.
  `pid` is truncated to `int32_t` before comparison.
- `WNOHANG` is honoured (`flags` is no longer ignored).
- The child exit status is stored in the new `task_t.exit_status` field by
  `sched_exit()` and returned through the `status` pointer.
- The parent blocks in `sched_wait_child()` (10 ms timeout) and is woken early
  by `sched_wake_child_waiter()` when a child exits.

## Scheduler support

- `task_t` gains `exit_status`.
- `sched_exit(status)` stores the status and calls `sched_wake_child_waiter()`
  to wake a parent sleeping on `EVENT_CHILD_EXIT`.
- New `sched_reap(tid, &status)` replaces `sched_cleanup()`; it reports whether
  a task was reaped, is still alive, or is already gone. It uses the effective
  status, so an exec wrapper that stays `TASK_DYING` because its replacement
  exited can still be reaped even when the idle reaper is starved.
- Removed the unreachable retry loops.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/syscall.c` | rewrote `k_waitpid()`; `base/kmalloc.h` include; `k_write()` throttle now uses the global HPET clock |
| `kernel/proc/sched.c` | `exit_status`, `sched_wake_child_waiter()`, `sched_wait_child()`, `sched_reap()` |
| `kernel/proc/sched.h` | `sched_wait_child()`, `sched_reap()` declarations |
| `kernel/proc/task.h` | `exit_status` field, `EVENT_CHILD_EXIT` |

## Testing

`ls` followed by `pwd` on `-smp 2` returns to the shell and reaps both the exec
wrapper and the `ls` task; the boot no longer respawns the shell.
