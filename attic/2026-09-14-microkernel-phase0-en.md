# 2026-09-14: Microkernel Phase 0 Kernel Mechanisms

## Overview

First step of the hybrid microkernel migration: land the kernel mechanisms that
later phases depend on, **without changing any observable behaviour**. No
service is moved to user space yet.

The full plan lives in the local design documents `design/microkernel-phase0-en.md`
and `design/microkernel-plan-phase0-1-en.md` (kept out of the repository).

## Deliverables

### uaccess (`kernel/mm/uaccess.{h,c}`)

- `user_range_ok()` validates a range against the task page tables (present +
  user-accessible page), which matters because user code is linked in the
  `0xffff800040000000` window while stacks are low.
- `copy_from_user` / `copy_to_user` / `clear_user` / `strncpy_from_user` for the
  current task, and `copy_from_task` / `copy_to_task` for cross-task IP copies
  (page-table walk + kernel HHDM).
- `vmm_query()` returns the raw leaf PTE; `vmm_map_task_phys()` maps a physical
  range into a task for future device/memory grants.

### Kernel object and handle model (`kernel/ipc/object.{h,c}`)

- `kernel_object_t`: type, atomic refcount, `impl`, `destroy`.
- Per-task `handle_table_t` with generation-tagged handles
  (`(generation << 16) | index`) so stale handles fail after slot reuse.
- `handle_alloc` / `handle_get` / `handle_close`; rights mask
  (`READ`/`WRITE`/`SEND`/`RECV`/`MAP`/`TRANSFER`).
- Wired into `task_make` (init), `task_fork` (reset) and `task_free` (destroy).

### IPC (`kernel/ipc/ipc.{h,c}`)

- `endpoint_t` = spinlock + 16-deep message queue; `ipc_msg_t` carries a tag,
  6 inline words and up to 2 transferred handles.
- `ipc_send` / `ipc_recv` / `ipc_recv_timeout` / `ipc_call` / `ipc_reply`.
- Cross-core blocking: `sched_wait_key()` / `sched_wake_key()` re-use the proven
  `sched_wake_*` scan pattern, with a timeout fallback so a lost wake cannot
  deadlock.

### notify (`kernel/proc/notify.{h,c}`)

Typed notification built on an endpoint (`notify_publish` / `notify_subscribe`);
the mechanism Phase 1 will use to deliver IRQs to servers.

### Service router (`kernel/service/service.{h,c}`)

`service_register` / `service_lookup` / `service_forward`. No service is
registered in Phase 0, so the router is dormant and syscalls take the in-kernel
path.

### New syscalls

`EP_CREATE 50`, `IPC_SEND 51`, `IPC_RECV 52`, `IPC_CALL 53`, `IPC_REPLY 54`,
`HANDLE_CLOSE 60`. Messages are marshalled through `uaccess`.

### Self-test

`kernel/ipc/selftest.{h,c}`, guarded by `ENABLE_MICROKERNEL_SELFTEST`, runs at
boot and exercises the message queue, the timeout path and the handle table,
logging `MK: selftest PASS` / `FAIL`.

### Conversions

`k_getcwd()` and `k_openat()` now go through the uaccess layer (the path is
copied into the kernel before use).

## Not yet done

- Converting the remaining user-buffer syscalls (`k_read`/`k_write` need bounce
  buffers; `k_chdir`/`k_unlink`/`...` paths; `execve` argv/envp; signals).
- `#PF` fixup for fault-safe copies.
- Wiring `syscall_funcs[]` through the router and reimplementing `eventbus` on
  `notify`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/mm/uaccess.{h,c}` | new user-access helpers |
| `kernel/mm/vmm.c`, `mm.h` | `vmm_query()`, `vmm_map_task_phys()` |
| `kernel/ipc/object.{h,c}` | kernel object + handle table |
| `kernel/ipc/ipc.{h,c}` | endpoints and IPC |
| `kernel/ipc/selftest.{h,c}` | boot self-test |
| `kernel/proc/notify.{h,c}` | notification primitive |
| `kernel/service/service.{h,c}` | service router scaffolding |
| `kernel/proc/task.{h,c}` | handle table, `wakeup_key`, `EVENT_IPC` |
| `kernel/proc/sched.{h,c}` | `sched_wait_key()` / `sched_wake_key()` |
| `kernel/proc/syscall.{h,c}` | IPC/handle syscalls; `k_getcwd`/`k_openat` uaccess |
| `kernel/kmain.c`, `kconfig.h` | optional self-test task |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- `ENABLE_MICROKERNEL_SELFTEST` prints `MK: selftest PASS` on `-smp 2` and
  `-smp 4` (3/3 on 4 CPUs).
- With the flag off, boot on `-smp 2` is clean and `ls` → `pwd` still works.
