# 2026-10-31: Zero misc clock broke PS/2 input

## Symptom

On the Skylake laptop the shell accepted no keys. A USB keyboard still worked. QEMU reproduced it on the PS/2 path: the kernel read the scancodes and delivered them, but the input server emitted no key.

## Root cause

`hpet_phys` was declared and read by `hpet_phys_base()` but never assigned in `hpet_init`. The misc server got no HPET mapping and `clock_gettime(CLOCK_MONOTONIC)` returned zero. The input server's `now_ms()` used that clock, so its key debounce `now_ms() - release_ms >= 25` never passed and every PS/2 key was dropped. USB HID is decoded in the kernel and does not use the clock.

## Changes

- hpet: store the physical base in `hpet_init`, so the misc grant maps the HPET window.
- misc: `misc_selftest` fails when the monotonic clock is zero.

## Verification

- GCC 10.2, TCG, `-smp 1`: PS/2 `sendkey` runs `ls`; `MK: selftest PASS`, `MK: misc PASS`, `MK: signalfd PASS`.
