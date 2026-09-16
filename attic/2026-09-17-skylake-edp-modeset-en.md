# 2026-09-17: Skylake eDP Mode Set

## Overview

Second step towards adaptive resolution: program the Skylake Gen9 display
pipeline on the primary eDP port (DDI-A) at the boot mode, instead of only
using the firmware aperture framebuffer.

## Changes

- `gfx_reg.h`: add the Gen9 mode-set registers (display-core power well,
  CDCLK, DPLL status, DDI buffer translation, DP transport control, transcoder
  DP control, universal-plane fields and `PIPE_MISC`).
- New `skl_display.{c,h}`: `skl_edp_set_mode()` allocates a GTT framebuffer,
  keeps the firmware display-core power, CDCLK, DPLL0 and trained eDP link,
  then programs the transcoder timing, pipe and plane, turns on the panel and
  backlight, and returns the framebuffer. `skl_display_dump()` logs the
  pipeline registers for debugging. Every hardware wait has a timeout.
- `display_mode.{c,h}`: expose the boot mode through `display_mode_get_boot()`.
- `gfx.c`: after the self-test, log the display state and attempt the mode set;
  on failure keep the firmware aperture framebuffer.

## Notes

- The mode set is display-only: it keeps the BIOS-trained eDP link and the
  display-core clocks, which avoids redoing link training and CDCLK changes.
- The scan-out framebuffer is a GTT buffer filled with a grey test pattern, so
  a successful mode set is visible; handing it to the console server is the
  next step.

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- QEMU is unchanged: the driver does not match QEMU's VGA device, so the mode
  set is not reached (no `GFX:` output) and the boot still reaches
  `name "hansh"`.
- Target Skylake/HD520 machine: verify `skl_display_dump()` values, the mode
  set (grey field scanned out) and the fallback when a step times out.
