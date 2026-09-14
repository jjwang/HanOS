# 2026-09-14: Server-Side Line Discipline

## Overview

Move the terminal line discipline (visible line length, backspace handling,
Enter and end-of-file) into the userspace input server. When the server owns
the keyboard, the kernel tty becomes a pass-through for the byte stream it
receives; the in-kernel driver keeps using the tty's own filter.

## Changes

- `userspace/input.c` tracks the number of visible characters on the current
  line and:
  - emits a backspace only when the line is non-empty;
  - resets the line on Enter;
  - resets the line and sends end-of-file on Ctrl-D;
  - echoes each emitted character through the console endpoint.
- `libc/bootinfo.h`: `INPUT_KEY_EOF` is now `0xFF` so it matches the kernel's
  `EOF` byte after the value has been truncated to eight bits.
- `kernel/fs/ttyfs.c`: when the input server is active the tty stores whatever
  byte it receives (the server already applied the discipline). The existing
  "visible length" backspace filter is kept for the in-kernel keyboard path.

## Files Changed

| File | Change |
|------|--------|
| `userspace/input.c` | line-length tracking, backspace/Enter/EOF discipline |
| `libc/bootinfo.h` | `INPUT_KEY_EOF` value aligned with the kernel |
| `kernel/fs/ttyfs.c` | pass through when the input server is active |

## Testing

- Server on: typing `l`, `x`, backspace, `s`, Enter runs `/bin/ls` (the
  backspace erases `x`), and a following `pwd` works.
- Server off: the same sequence with the in-kernel keyboard still runs
  `/bin/ls`.
- Warning-free full build.
