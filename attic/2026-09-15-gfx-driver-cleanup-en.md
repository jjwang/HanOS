# 2026-09-15: GPU Driver Cleanup

## Overview

Harden the Intel display driver for the Skylake HD520 target: correct the
chipset and device identification, repair corrupted comments, consolidate the
boot self-test, and fix register offsets and log formats.

## Changes

- `gfx.c`: identify the PCH by scanning the enumerated PCI devices for Sunrise
  Point (0x9D00) instead of assuming the 00:1f.0 ISA bridge; rename the
  misnamed `DEVICE_SUNRISE_PANTHERPOINT`; match only HD520 (0x1916) and drop
  the Broadwell HD5500 id that could never pass the Skylake PCH check.
- `gfx.c`: replace the corrupted "Claude Code" text with "cursor" (four
  comments and the `gfx_configure_cursor()` name in a log line).
- `gfx.c`: move the boot feature test out of `gfx_init()` into
  `gfx_test_advanced_features()` (previously dead code) and run it from
  `gfx_init()`; all tests now share a single force-wake session. `gfx_init()`
  keeps detection, BAR/GTT/memory setup and the aperture framebuffer
  backbuffer.
- `gfx.c` and `gfx_reg.h`: fix `gfx_set_plane_fb()` to write the surface
  address to `DSPxSURF` (offset 0x1C) instead of `DSPxLINOFF` (0x04); add
  `PIPE_*` and `DSP_SURF`/`DSP_STRIDE` macros and use them in the timing and
  plane helpers.
- `gfx.c`: log 32-bit registers and values with `%u`/`%x` instead of
  `%ld`/`%lx`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/display/gfx.c` | chipset/device id, self-test consolidation, register offsets, log formats |
| `kernel/device/display/gfx_reg.h` | pipe timing and plane surface macros |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- QEMU boots unchanged: the driver does not match QEMU's VGA device, so it
  performs no register access (no `GFX:` output) and the boot still reaches
  `name "hansh"`.
- Target Skylake/HD520 machine: verify the device and chipset detection, the
  GTT/stolen parameters and the self-test results.
