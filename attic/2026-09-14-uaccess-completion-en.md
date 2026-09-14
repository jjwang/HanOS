# 2026-09-14: Complete User-Buffer Syscall Conversion

## Overview

Finish moving every syscall that touches a user buffer onto the user-access
helpers. Previously the process, signal, time and misc handlers still
dereferenced user pointers directly.

## Converted handlers (`kernel/proc/syscall.c`)

- `k_execve`: the path, the `argv`/`envp` pointer arrays and every string they
  reference are copied into kernel memory first (new `copy_exec_argv()` /
  `free_exec_argv()` helpers, bounded to 32 entries) before `sched_execve()` runs.
- `k_sigprocmask`, `k_sigaction`: the signal set and action structures are
  copied in and out with `copy_from_user` / `copy_to_user`.
- `k_getclock`: builds the time in a kernel buffer and copies it out.
- `k_getrusage`: zeroes the structure with `clear_user`.
- `k_getentropy`: fills a kernel buffer and copies it out.
- `k_debug_log`, `k_runcmd`: copy the user string first.
- `k_pipe`: writes the two descriptors with `copy_to_user`.
- `k_futex_wait`, `k_futex_wake`: read the futex word with `copy_from_user`.
- `k_waitpid`: writes `status` with `copy_to_user` / `clear_user`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/syscall.c` | convert the remaining user-buffer handlers; `copy_exec_argv` helper |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- The shell still runs `ls` and `pwd`, exercising `execve`, `waitpid`, file I/O
  and the signal/time paths.
