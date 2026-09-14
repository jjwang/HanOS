# 2026-09-14: Userspace Console Output and Server-Side Echo

## Overview

Give userspace a way to write to the kernel terminal and move keyboard echo out
of the kernel, so the input server can echo the keys it decodes.

## Console write path

- `libc/bootinfo.h` gains a `console_ep` field and the `CONSOLE_WRITE_TAG`.
- `kernel/ipc/input_srv.c` creates a third endpoint, `console_ep`, and starts a
  small kernel task (`conkbd`) that receives `CONSOLE_WRITE_TAG` messages and
  writes the byte to the terminal with `kprintf("%c", ...)`.
- The spawn hook attaches a SEND handle for `console_ep` to the input server's
  bootinfo.

## Echo moved to the input server

- `userspace/input.c` echoes each decoded key (including backspace) through the
  console endpoint before forwarding it to the kernel.
- `kernel/fs/ttyfs.c` skips its own echo while the input server is active, so a
  key is echoed exactly once. The in-kernel echo remains the fallback when the
  server is disabled.

## Files Changed

| File | Change |
|------|--------|
| `libc/bootinfo.h` | `console_ep` handle and `CONSOLE_WRITE_TAG` |
| `kernel/ipc/input_srv.c` | console endpoint, `conkbd` task, handle grant |
| `kernel/fs/ttyfs.c` | skip echo when the input server is active |
| `userspace/input.c` | echo decoded keys through the console endpoint |

## Testing

- With `ENABLE_INPUT_SERVER` enabled, the keyboard works through the server and
  the shell accepts commands on `-smp 2` and `-smp 4`.
- With the switch off, the in-kernel keyboard driver and its echo are used.
