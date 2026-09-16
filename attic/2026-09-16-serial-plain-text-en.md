# 2026-09-16: Plain-Text Serial Output

## Overview

Serial monitors that do not interpret ANSI escapes showed the kernel log as
boxes and question marks, because every line carried SGR colour codes. Strip
the ANSI escape sequences from the serial stream.

## Changes

- `kernel/base/klog.c`: pass the serial stream through `serial_keep()`, which
  removes ANSI CSI sequences (`ESC [ ... final byte`) before `serial_write()`.
  State is kept across the line so a sequence is removed even if it spans
  characters. The kernel terminal and console server still receive the coloured
  output.

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- QEMU serial log now contains zero ESC bytes and no other non-printable bytes;
  the lines are plain readable text, and the boot still reaches
  `name "hansh"` with no panic.
