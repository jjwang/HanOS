# 2026-09-14: Userspace Console Server

## Overview

Move framebuffer ownership and text rendering out of the kernel. A userspace
`/bin/console` server maps the framebuffer, keeps a back buffer and renders the
kernel's console output; the kernel forwards its output to the server and stops
touching the framebuffer.

## Console server (`userspace/console.c`)

- Receives console bytes on an endpoint granted by the kernel.
- Keeps a back buffer, renders with the shared gohufont glyphs and blits the
  buffer to the framebuffer after draining its queue.
- Handles newline, carriage return, backspace, tab, line scrolling and a
  minimal ANSI SGR colour subset (so the kernel's coloured output renders).
- The font is linked from `userspace/font.asm`, which includes the same PSF
  glyphs as the kernel terminal.

## Kernel plumbing (`kernel/ipc/console_srv.{h,c}`)

- Creates the console endpoint and spawns `DEFAULT_CONSOLE_SVR`.
- A spawn hook maps the framebuffer physical range into the new task with user
  access and write-combining, and passes the framebuffer geometry, cursor
  position and colours through `bootinfo`.
- `console_write_buf()` forwards a byte buffer to the server, chunked into IPC
  messages, and reports when the server is not active so the caller can fall
  back to the in-kernel terminal.

## Forwarding

- `kprintf()` (`kernel/base/klog.c`) sends its formatted buffer to the console
  server when it is active and skips the in-kernel `term_putch()`/`term_refresh()`.
- `kernel/fs/ttyfs.c` and the `kupdateui` task skip their cursor/refresh calls
  while the server owns the display.

## Interface additions

- `libc/bootinfo.h`: framebuffer virtual address, width, height, pitch, size,
  cursor position and colours.
- `IPC_RECV_NB` (64): non-blocking receive, used by the server to drain its
  queue before one blit.
- `term_get_pos()` accessor; `IPC_QUEUE_LEN` raised to 64.

## Configuration

`ENABLE_CONSOLE_SERVER` (off by default) and `DEFAULT_CONSOLE_SVR` in
`kernel/kconfig.h`; `kmain` starts the server from `kshell`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/ipc/console_srv.{h,c}` | spawn the console server and forward output |
| `kernel/base/klog.c` | forward `kprintf` output to the server |
| `kernel/fs/ttyfs.c`, `kernel/kmain.c` | skip in-kernel terminal calls when active |
| `kernel/device/display/term.{h,c}` | `term_get_pos()` accessor |
| `kernel/proc/syscall.{h,c}` | `IPC_RECV_NB` |
| `kernel/ipc/ipc.h` | larger endpoint queue |
| `libc/bootinfo.h`, `libc/sysfunc.{h,c}` | framebuffer fields and `sys_ipc_recv_nb` |
| `userspace/console.c`, `userspace/font.asm` | the server and its font |
| `userspace/GNUmakefile` | build `console` |

## Testing

- With the console server enabled, screendumps show rendered text and change
  after running shell commands; no faults occur.
- Input and console servers together work on `-smp 2` and `-smp 4`.
- With both disabled the in-kernel terminal and keyboard path are unchanged.
