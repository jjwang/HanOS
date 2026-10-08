# 2026-10-08: In-place execve and the syscall kernel-stack switch

## Symptom

The interactive shell hangs after a game exits. Play `tetris` about 15 seconds, press `q`, and the prompt does not return in a long session. The shell blocks in `wait`.

## Background

`execve` spawned a new process and made the caller exit. The parent forked a child; the child ran the image in a grandchild and exited. The parent waited on the grandchild through the process server, whose shadow state (`procs[]`) tracked exit and child counts. When that bookkeeping disagreed with the kernel, the wait never completed.

The fix runs `execve` in the calling process. The image is replaced in place, the pid stays the same, and the parent waits on the process it forked.

## Root causes

Three faults blocked the in-place path. Each one produced a distinct failure.

### 1. The syscall handler ran on the user stack

A probe showed the syscall handler used the caller's user stack. Changing the address space mid-syscall unmapped the kernel's own stack and triple-faulted. `syscall_handler` now saves the user RSP to `gs:16`, switches RSP to `gs:24` (the kernel stack top), and builds the frame there.

### 2. The FS base (TLS) carried over

`sched.c` reloads FS only when `next->fs_base != 0`. An in-place exec kept the old image's TLS base, so the new image ran with the wrong thread pointer and jumped to a low address (`0x267`). exec now clears the FS base.

### 3. The per-CPU syscall frame was repointed while blocked

`cpu->syscall_frame` is one slot per CPU. `elf_load` reads the image through the filesystem servers and blocks; other syscalls on the same CPU repoint the slot. Taking the frame after the block targeted another process's frame, so the fault address and pid varied run to run. The in-place path takes the frame before the block; the frame lives on the caller's kernel stack and survives it.

## Implementation

### Kernel-stack switch (f4d2e85)

- `syscall_handler.asm` saves the user RSP to `gs:16` and sets RSP to `gs:24`.
- `cpu_t` gains `syscall_user_rsp` (offset 16) and `syscall_kstack` (offset 24).
- `do_context_switch` sets `cpu->syscall_kstack = next->kstack_top`.

### In-place exec (8700a06)

- `sched_execve_prep` builds a fresh process: address space, loaded image, and user stack.
- It takes a `fork_fds` flag.
- The in-place path passes `false` and skips the fd clone, so the process server does not hold a second copy of the tty endpoint.
- `sched_execve_inplace` moves the prepared process's user state (address space, context, user stack) into the current process.
- It then switches CR3, frees the old address space, clears the FS base, and rewrites the syscall return frame.
- `k_execve` calls `sched_execve_inplace` for user processes. The spawn path stays as fallback.
- `process_free_addrspace` frees an address space and its mappings; `process_free` and the in-place path call it.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/syscall_handler.asm` | switch to the kernel stack on syscall entry |
| `kernel/arch/x64/smp.h` | add `syscall_user_rsp`, `syscall_kstack` |
| `kernel/proc/sched.c` | set `syscall_kstack`; add `sched_execve_prep`, `sched_execve_inplace` |
| `kernel/proc/sched.h` | declare `sched_execve_inplace` |
| `kernel/proc/syscall.c` | call the in-place path from `k_execve` |
| `kernel/proc/process.c` | add `process_free_addrspace` |
| `kernel/proc/process.h` | declare `process_free_addrspace` |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- Boot on `-smp 2` and `-smp 4`: `selftest PASS`, no faults.
- Interactive `ls /`, `pwd`, `ls /bin`, and pipelines: correct, no faults.
- Loop `tetris` then `q` 20 times: the prompt returns every time.
- `musltest` completes: `pipe-ok`, `nanosleep ok`, `mustest: done`.

## Not yet done

- The in-place path allocates a temporary process per exec. Replace it with a direct reset of the current process.
- The process server still keeps shadow state. Move fork/exec/exit/wait authority into the kernel.
