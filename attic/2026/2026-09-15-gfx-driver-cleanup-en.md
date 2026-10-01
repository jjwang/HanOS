# 2026-09-15: GPU Driver Cleanup

## Overview

Harden the Intel display driver for the Skylake HD520 target.

- Correct the chipset and device identification.
- Repair corrupted comments.
- Consolidate the boot self-test.
- Fix register offsets and log formats.

## Changes

- `gfx.c`: scan the enumerated PCI devices to find the PCH (Sunrise Point, 0x9D00). Drop the 00:1f.0 ISA bridge assumption. Rename the misnamed `DEVICE_SUNRISE_PANTHERPOINT`. Match only HD520 (0x1916). Drop the Broadwell HD5500 id; it never passed the Skylake PCH check.
- `gfx.c`: replace the corrupted "Claude Code" text with "cursor". Fix four comments and the `gfx_configure_cursor()` name in a log line.
- `gfx.c`: move the boot feature test out of `gfx_init()` into `gfx_test_advanced_features()` (previously dead code). Run it from `gfx_init()`. All tests share one force-wake session. `gfx_init()` keeps detection, BAR/GTT/memory setup and the aperture framebuffer backbuffer.
- `gfx.c` and `gfx_reg.h`: fix `gfx_set_plane_fb()` to write the surface address to `DSPxSURF` (offset 0x1C), not `DSPxLINOFF` (0x04). Add `PIPE_*` and `DSP_SURF`/`DSP_STRIDE` macros. Use them in the timing and plane helpers.
- `gfx.c`: log 32-bit registers and values with `%u`/`%x`, not `%ld`/`%lx`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/display/gfx.c` | chipset/device id, self-test consolidation, register offsets, log formats |
| `kernel/device/display/gfx_reg.h` | pipe timing and plane surface macros |

## Testing

- `make -C kernel clean && make -C kernel` builds with no warnings and the full image builds.
- QEMU boots unchanged. The driver does not match QEMU's VGA device, so it performs no register access (no `GFX:` output). Boot still reaches `name "hansh"`.
- Target Skylake/HD520 machine: verify device and chipset detection, the GTT/stolen parameters and the self-test results.
