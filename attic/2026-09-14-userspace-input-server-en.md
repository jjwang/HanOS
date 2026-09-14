# 2026-09-14: Userspace Input Server

## Overview

The first userspace device server. The PS/2 keyboard interrupt and its I/O
ports are handed to `/bin/input`, which decodes scancodes and sends characters
back to the kernel over IPC. Keyboard input no longer runs in the kernel when
the server is enabled.

## Shared keycode table

`kernel/device/keyboard/keycode.{c,h}` moved to `libc/`, so the in-kernel
fallback driver and the userspace server use the same scancode->ASCII table.

## Boot protocol (`libc/bootinfo.h`)

`bootinfo_t` carries the resources the kernel grants a server: the two endpoint
handles (interrupt notifications in, decoded keys out), the interrupt line, and
the granted I/O-port ranges. It also defines the shared message tags.

## Kernel changes

- `task_t.bootinfo` holds the server's startup block; it is released in
  `task_free()`.
- `SYS_BOOTINFO` (63) copies the current task's bootinfo to user space.
- `sched_set_spawn_hook()` runs a callback inside `sched_execve()` **before**
  the new task becomes runnable, so the spawner can attach handles, port
  ranges and bootinfo without racing with the task.
- `kernel/ipc/input_srv.{h,c}`:
  - creates the IRQ object for line 1 and binds it to a notification endpoint;
  - creates the notification and key endpoints;
  - spawns `DEFAULT_INPUT_SVR` and attaches handles/ports/bootinfo via the
    spawn hook;
  - starts a small kernel task (`inkbd`) that relays decoded keys onto the
    event bus, so the terminal path is unchanged.
- `kconfig.h`: `DEFAULT_INPUT_SVR` is `/bin/input`, plus a new
  `ENABLE_INPUT_SERVER` switch (off by default; the in-kernel driver is used
  when off).
- `kmain`: start the input server from `kshell` when enabled.

## Userspace server (`userspace/input.c`)

Waits for an interrupt notification, drains the controller through
`sys_ioport_access`, decodes make/break scancodes (tracking shift, caps lock and
ctrl, including Ctrl-D as EOF) and sends each key to the kernel with
`sys_ipc_send`. Built to `/bin/input` via the userspace makefile.

## User/kernel interface

`libc/sysfunc.{h,c}` gained `sys_bootinfo()` alongside the earlier IPC, IRQ and
I/O-port wrappers.

## Files Changed

| File | Change |
|------|--------|
| `libc/keycode.{c,h}` | shared scancode table (moved from the kernel) |
| `libc/bootinfo.h` | bootinfo block and message tags |
| `kernel/ipc/input_srv.{h,c}` | spawn and drive the input server |
| `kernel/proc/task.{h,c}` | per-task bootinfo |
| `kernel/proc/syscall.{h,c}` | `SYS_BOOTINFO` |
| `kernel/proc/sched.{h,c}` | spawn hook |
| `kernel/ipc/irq.c` | IRQ delivery (unchanged interface) |
| `kernel/kmain.c`, `kconfig.h` | start the server when enabled |
| `userspace/input.c`, `userspace/GNUmakefile` | the server |
| `libc/sysfunc.{h,c}` | `sys_bootinfo()` |

## Testing

- With `ENABLE_INPUT_SERVER` enabled: `input: server started` is logged and the
  keyboard works through the server (`ls` -> `pwd`) on `-smp 2` and `-smp 4`.
- With the switch off: the in-kernel keyboard driver handles line 1 and the
  shell still works.
