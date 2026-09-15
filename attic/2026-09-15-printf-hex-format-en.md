# 2026-09-15: Standard Hex Format Specifiers

## Overview

Align the hex format specifiers with the C standard. The `%x`/`%X` conversions
already matched the standard; the deviations were `%p` and a few call sites
that used `%x` for 64-bit addresses.

## Changes

- `libc/printf.c`: `%p` now prints `0x` followed by lowercase hex digits
  instead of zero-padded uppercase without a `0x` prefix, which is the value
  the standard leaves implementation-defined but matches the common
  convention.
- Fix the call sites that printed a 64-bit address with `%x`, which consumes
  an `unsigned int` and silently drops the high bits: `pmm.c`, `gfx.c` and
  `elf.c` now use `%lx` (and cast the pointer to `uint64_t` where needed).

## Testing

- The formatter was compared against glibc on the host for `%x`, `%X`, `%#x`,
  `%lx`, `%llx`, width, precision and `%p`; all match.
- Clean kernel build; SMP2 boot and interactive `ls`/`pwd` pass.
