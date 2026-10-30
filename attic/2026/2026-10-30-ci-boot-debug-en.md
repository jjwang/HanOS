# 2026-10-30: GitHub Actions boot test and the two faults it exposed

## Symptom

The GitHub Actions boot job stays red. Early runs stop with an empty serial log. Later runs boot to the shell but report `MK: selftest FAIL`.

## Background

The workflow runs on `ubuntu-24.04`. It builds the kernel and userspace through the host GCC wrapper, builds `cdrom.iso`, then boots it under QEMU 8.2 with TCG. The boot script waits for `name "hansh"` and `MK: selftest PASS` in the serial log.

## Root causes

### 1. The build-id note broke the ELF layout

Ubuntu host GCC emits `.note.gnu.build-id` by default. `kernel/linker.ld` does not place that section. `ld` produced a data segment that overlaps the text segment. Limine refused to load the kernel and the serial log stayed empty. The bare-metal cross GCC does not emit the note, so the development machine hid the fault.

Fix: pass `-Wl,--build-id=none` when linking the kernel (`c13d3ee`).

### 2. The page-fault entry clobbered `%rax`

The selftest failed in its fault-safe copy check. `copy_from_user` reported `EFAULT`; `copy_to_user` returned success on the same unmapped address. Instrumentation showed the write did fault, the handler found the fixup, and the flag slot held 1. gdb then showed `%rax = 0` at the fixup, though the store ran with `%rax = len`.

The `exc14` entry clears `%fs` through `%rax` before `pusham` saves the context. The interrupted `%rax` became 0. `copy_to_user` kept `len` in `%rax` across the store, so the return value computed `0 + 8 - 8 = 0`.

Fix: save and restore `%rax` around the segment reset in `isr_base.s` (`9cf3a0a`).

## Diagnosis path

- Host GCC wrapper and the nolibc cross toolchain both failed. The nolibc GCC 16.2 `stdint.h` delegated to the host glibc; the kernel `-MM` rule lacked `-ffreestanding`.
- The cross toolchain detour was dropped. The host GCC path works once the build-id is disabled.
- The CI artifact was reproduced locally with host GCC 10.5: ISO 9.85 MB and initrd 5.6 MB. The image failed to boot before the build-id fix and booted after it.
- The copy failure was reproduced locally with GCC 13.1 from the focal toolchain PPA, which runs on the development host. gdb watched the store, the fixup, and the flag slot.

## Implementation

- `c13d3ee fix(kernel): disable build-id so Limine loads the ELF`
- `956c795 ci: build and boot the ISO on GitHub Actions`
- `9cf3a0a fix(arch): preserve rax across the page-fault entry`
- `ac1cb94 test(kernel): simplify the uaccess self-test`
- `c873398 ci: print the serial log after a passing boot test`
- The selftest probes `0x00007ffff0000000`, away from the low 1 MiB that SMP bring-up identity-maps.
- The boot script prints the serial log after a passing run.

## Verification

- GCC 13.1, TCG, `-smp 1`: `MK: selftest PASS`; the shell starts as `hansh`.
- Cross GCC 10.2, TCG, `-smp 1`: `MK: selftest PASS`; the shell starts as `hansh`.
