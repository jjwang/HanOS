# 2026-09-16: Log Format Spacing

## Overview

Remove the space padding left by the earlier printf-specifier refactor, so
values print without stray leading spaces (for example `model 0x a` becomes
`model 0xa`).

## Changes

- Hex identifiers now use zero padding where the width matters
  (`%02x:%02x.%01x`, `%04x:%04x`, `%02x`, `%03x`, `%04x`).
- Addresses drop the artificial width (`%11lx`/`%11x` become `%lx`/`%x`).
- Small decimal fields use no width (`%2d`, `%4d`, `%8d`, `%11d` become `%d`).
- Files: `kernel/sys/cpu.c`, `kernel/sys/pci.c`,
  `kernel/device/storage/ata.c`, `kernel/device/display/gfx.c`,
  `kernel/proc/syscall.c`, `kernel/proc/elf.c`, `kernel/fs/fat32.c`,
  `kernel/fs/vfs.c`, `kernel/mm/pmm.c`, `kernel/mm/buddy.c`, `kernel/kmain.c`.

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- The serial log now shows `CPU 0: model 0xa, family 0x6` and
  `PCI: 00:00.0 - 8086:29c0`; the boot banner shows `Monitor : 40 x 30 cm` and
  `Memory : 1068 MB`; the boot reaches `name "hansh"` with no panic.
