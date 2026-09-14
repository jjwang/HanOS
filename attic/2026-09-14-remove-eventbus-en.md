# 2026-09-14: Remove the Legacy Event Bus

## Overview

The event bus had become a thin wrapper over the notification primitive, so it
is removed and callers use notifications directly. This drops a layer and its
no-op dispatch hook.

## Changes

- Delete `kernel/proc/eventbus.{c,h}` and the `eb_publish` / `eb_subscribe` /
  `eb_dispatch` / `eb_init` API.
- `kernel/proc/notify.{h,c}` gains a shared notification object
  (`notify_system`) and `notify_system_init()`, initialised once the allocator
  is ready.
- Call sites now use notifications:
  - `kernel/device/keyboard/keyboard.c` and `kernel/ipc/input_srv.c` publish
    with `notify_publish(&notify_system, EVENT_KEY_PRESSED, ...)`;
  - `kernel/fs/ttyfs.c` subscribes with
    `notify_subscribe(&notify_system, EVENT_KEY_PRESSED, ...)`;
  - `kernel/kmain.c` calls `notify_system_init()`.
- `kernel/proc/sched.c`: the `eb_dispatch()` call in `do_context_switch()` is
  gone, along with the now-unused `sched_wait_event()` and
  `sched_resume_event()`.
- `kernel/proc/syscall.c`: drop the unused event bus include.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/eventbus.{c,h}` | deleted |
| `kernel/proc/notify.{h,c}` | shared notification object and init |
| `kernel/device/keyboard/keyboard.c` | publish via notify |
| `kernel/fs/ttyfs.c` | subscribe via notify |
| `kernel/ipc/input_srv.c` | relay via notify |
| `kernel/kmain.c` | `notify_system_init()` |
| `kernel/proc/sched.{c,h}` | drop `eb_dispatch()` and the unused wait/resume helpers |
| `kernel/proc/syscall.c` | drop the unused include |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free; the full image
  builds.
- In-kernel keyboard (server off): `ls` -> `pwd` works.
- Userspace input server (server on): `ls` -> `pwd` works on `-smp 2` and
  `-smp 4`.
