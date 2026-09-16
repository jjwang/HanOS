# 2026-09-16: Console Server Starts from a Clean Screen

## Overview

Fix the boot splash (the HNK logo) and stale kernel terminal content showing up
in the userspace console. The console server inherited the kernel framebuffer,
and if the kernel blitted its own terminal buffer after the server had copied
it, the removed logo (or old content) reappeared.

## Changes

- `userspace/console.c`: clear the back buffer at startup and blit it, instead
  of copying the kernel framebuffer. The server owns the display, so it should
  not inherit the boot log or splash.
- `kernel/kmain.c`: call `term_refresh()` only when the console server is not
  active, so the kernel no longer blits its own terminal back buffer over the
  server's screen.

## Testing

- `make -C kernel clean && make -C kernel` and the userspace build are
  warning-free; the full image builds.
- QEMU: after booting and running several commands the screen shows only the
  banner, prompt and command output, with the lower half black; the shell still
  runs `ls` and `pwd` and the boot reaches `name "hansh"` with no panic.
