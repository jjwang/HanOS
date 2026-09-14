# 2026-09-15: Input and Console Servers by Default

## Overview

Make the userspace input and console servers the default keyboard and display
path, and fix a stall that appeared once the console server owned the screen.

## Changes

- `kernel/kconfig.h`: `ENABLE_INPUT_SERVER` and `ENABLE_CONSOLE_SERVER` now
  default to `true`, so the service form is used unless explicitly disabled.
- `kernel/ipc/console_srv.c`: `kprintf()` output is buffered in a ring and
  forwarded by a dedicated kernel thread (`conflush`). A syscall runs with
  interrupts disabled, so the previous busy-wait held the CPU and starved the
  console server, wedging the shell. Logging is now non-blocking and the
  forwarder yields while the server drains.
- `userspace/console.c`: track dirty rows and copy only those to the
  framebuffer instead of the whole screen on every batch.

## Files Changed

| File | Change |
|------|--------|
| `kernel/kconfig.h` | enable the input and console servers by default |
| `kernel/ipc/console_srv.c` | ring buffer plus forwarding thread for console output |
| `userspace/console.c` | dirty-row framebuffer blit |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- Headless SMP2 and SMP4 boots reach `name "hansh"` with no panic.
- The shell runs `ls` (correcting a mistyped character with backspace) and
  `pwd` through the servers, and a framebuffer dump shows the banner, prompt
  and directory listing.
