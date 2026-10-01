# 2026-09-14: Microkernel Kernel Foundations (uaccess, handles, IPC)

## Overview

Add the kernel mechanisms that the hybrid microkernel migration depends on, **without changing any observable behaviour**. No service moves to user space yet.

Keep the migration plan as local design notes (not part of the repository).

## Deliverables

### uaccess (`kernel/mm/uaccess.{h,c}`)

- `user_range_ok()` validates a range against the task page tables (present + user-accessible page). User code links in the `0xffff800040000000` window while stacks stay low.
- `copy_from_user` / `copy_to_user` / `clear_user` / `strncpy_from_user` for the current task; `copy_from_task` / `copy_to_task` for cross-task IP copies (page-table walk + kernel HHDM).
- `vmm_query()` returns the raw leaf PTE; `vmm_map_task_phys()` maps a physical range into a task for future device/memory grants.

### Kernel object and handle model (`kernel/ipc/object.{h,c}`)

- `kernel_object_t`: type, atomic refcount, `impl`, `destroy`.
- Per-task `handle_table_t` with generation-tagged handles (`(generation << 16) | index`). Stale handles fail after slot reuse.
- `handle_alloc` / `handle_get` / `handle_close`; rights mask (`READ`/`WRITE`/`SEND`/`RECV`/`MAP`/`TRANSFER`).
- Wired into `task_make` (init), `task_fork` (reset) and `task_free` (destroy).

### IPC (`kernel/ipc/ipc.{h,c}`)

- `endpoint_t` = spinlock + 16-deep message queue; `ipc_msg_t` carries a tag, 6 inline words and up to 2 transferred handles.
- `ipc_send` / `ipc_recv` / `ipc_recv_timeout` / `ipc_call` / `ipc_reply`.
- Cross-core blocking: `sched_wait_key()` / `sched_wake_key()` re-use the proven `sched_wake_*` scan pattern, with a timeout fallback so a lost wake cannot deadlock.

### notify (`kernel/proc/notify.{h,c}`)

Typed notification built on an endpoint (`notify_publish` / `notify_subscribe`). The legacy event bus (`kernel/proc/eventbus.c`) is now a thin wrapper over it: `eb_publish`/`eb_subscribe` send/receive notifications, `eb_dispatch` is a no-op and the keyboard path no longer polls. `eb_init()` runs from `kmain` once the allocator is ready.

### Service router (`kernel/service/service.{h,c}`)

`service_register` / `service_lookup` / `service_forward`. No service registers yet, so the router stays dormant and syscalls take the in-kernel path.

### New syscalls

`EP_CREATE 50`, `IPC_SEND 51`, `IPC_RECV 52`, `IPC_CALL 53`, `IPC_REPLY 54`, `HANDLE_CLOSE 60`. Messages are marshalled through `uaccess`.

### Self-test

`kernel/ipc/selftest.{h,c}`, guarded by `ENABLE_MICROKERNEL_SELFTEST`, runs at boot and exercises the message queue, the timeout path and the handle table, logging `MK: selftest PASS` / `FAIL`.

### Conversions

The user-buffer syscalls now go through the uaccess layer: `k_openat`, `k_getcwd`, `k_chdir`, `k_unlink`, `k_chmod`, `k_faccessat`, `k_fstatat`, `k_readlink` copy paths/strings into the kernel first (a new `copy_user_path()` helper), while `k_read`, `k_write`, `k_fstat` and `k_readdir` validate the buffer and use `copy_to_user`/`clear_user` for output.

## Not yet done

- `k_execve` argv/envp and the signal structures are still accessed directly.
- `#PF` fixup for fault-safe copies — the current copies validate the range before dereferencing but do not recover from a fault.
- Wiring `syscall_funcs[]` through the service router (the router module is implemented but no syscall calls it yet).

## Files Changed

| File | Change |
|------|--------|
| `kernel/mm/uaccess.{h,c}` | add user-access helpers |
| `kernel/mm/vmm.c`, `mm.h` | add `vmm_query()`, `vmm_map_task_phys()` |
| `kernel/ipc/object.{h,c}` | add kernel object and handle table |
| `kernel/ipc/ipc.{h,c}` | add endpoints and IPC |
| `kernel/ipc/selftest.{h,c}` | add boot self-test |
| `kernel/proc/notify.{h,c}` | add notification primitive |
| `kernel/proc/eventbus.{h,c}` | reimplement event bus over notify; add `eb_init()` |
| `kernel/service/service.{h,c}` | add service router scaffolding |
| `kernel/proc/task.{h,c}` | add handle table, `wakeup_key`, `EVENT_IPC` |
| `kernel/proc/sched.{h,c}` | add `sched_wait_key()` / `sched_wake_key()` |
| `kernel/proc/syscall.{h,c}` | add IPC/handle syscalls; route `k_getcwd`/`k_openat` through uaccess |
| `kernel/kmain.c`, `kconfig.h` | add optional self-test task |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- `ENABLE_MICROKERNEL_SELFTEST` prints `MK: selftest PASS` on `-smp 2` and `-smp 4` (3/3 on 4 CPUs).
- With the flag off, boot on `-smp 2` is clean and `ls` → `pwd` still works.
