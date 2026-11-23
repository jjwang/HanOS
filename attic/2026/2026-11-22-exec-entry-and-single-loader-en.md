# Validate the ELF entry and keep one loader

## Problem

The stress run under g++ load killed a `cc1plus` with a page fault at RIP 0 and addr 0. The chain: `PROC_EXEC` was forwarded to the process server with the default 1 s router timeout; mapping a large `cc1plus` can take longer, so `exec_via_server` failed and `k_execve` fell back to the in-kernel `sched_execve`, creating a second process while the process server had already spawned one. The process server's `exec_load` also used `eh->entry` without checking it, so a truncated or corrupt ELF started a process at RIP 0. On any failure after `sys_proc_spawn` it returned the pid, so a half-set-up process looked like success.

## Fix

- `exec_load` requires `eh->entry` to fall inside a `PT_LOAD` segment, and returns -1 unless the process was fully mapped and started.
- `k_execve` runs only the process server when it is registered; a failure returns `ENOEXEC` instead of falling back to a second loader.
- `PROC_EXEC` waits for the process server to finish instead of abandoning it after 1 s.

## Verification

Three stress runs pass with no page fault, no `router_forward(SVC_PROC)` timeout and no `VFS open` failure.
