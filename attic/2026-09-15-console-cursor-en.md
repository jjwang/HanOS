# 2026-09-15: Console Server Cursor

## Overview

Restore the blinking cursor now that the userspace console server owns the
screen; the server previously rendered text but never drew a cursor.

## Changes

- `userspace/console.c`: draw a block cursor in the next cell and blink it
  every 500 ms, keeping it solid while output is being drawn. Rendering was
  split into `draw_cell()` so any cell can be drawn, including the cursor.
- `kernel/proc/syscall.{c,h}` and `libc/sysfunc.{c,h}`: add `IPC_RECV_TIMEOUT`
  (syscall 65), a receive with a millisecond timeout that the server uses to
  drive the blink.

## Files Changed

| File | Change |
|------|--------|
| `userspace/console.c` | cursor rendering and blink |
| `kernel/proc/syscall.{c,h}` | `IPC_RECV_TIMEOUT` handler and number |
| `libc/sysfunc.{c,h}` | `sys_ipc_recv_timeout` wrapper |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- Framebuffer dumps taken at intervals show the cursor toggling on the prompt
  line; SMP2 and SMP4 boots and interactive `ls`/`pwd` pass.
