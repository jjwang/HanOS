# 2026-09-22: Skylake eDP Mode Set

## Overview

Program the Skylake Gen9 display pipeline on the primary eDP port (DDI-A) at
the boot mode, hand the scanned framebuffer to the console server, and fix the
issues found on the target HD520 machine: a moving ripple on the panel and an
image that was stuck at the firmware's 800x600 source region.

## Changes

- `gfx_reg.h`: add and correct the Gen9 mode-set registers — eDP transcoder
  timing, source copies and DP M/N; the eDP transcoder's shifted pipe config
  (`0x7F008`); the pipe-A scalers; `DC_STATE_EN`; the `DP_TP_CTL` link-train and
  enhanced-framing bits; the plane data-buffer and watermark fields; the AUX
  control fields.
- `skl_display.c`:
  - Tear down the inherited pipeline once at takeover in reverse order
    (`plane -> pipe -> transcoder -> DDI -> PLL`). `wait_pipe_off()` confirms the
    pipe really stopped — the pipe state bits are a fast path, the frame counter
    is the direct evidence — and a failed confirmation aborts the mode set
    instead of touching the clock tree with a live pipe.
  - Keep the firmware's eDP DP M/N and `PIPE_MISC`. The panel runs at 6bpc
    (18bpp), so the firmware's data M/N and colour depth must be preserved;
    recomputing the data M/N for 24bpp desynchronised the sink and produced
    moving horizontal ripples.
  - Restore the sink's negotiated enhanced-framing bit in `DP_TP_CTL`.
  - Program the eDP transcoder's shifted pipe config (`0x7F008`) and the source
    size on the pipe and both eDP copies. The source is written before the
    transcoder timings and re-armed (together with `PLANE_SIZE`/`PLANE_SURF`) at
    a vblank once the pipe is running, because these values are double-buffered
    and only take effect when armed.
  - Write `PLANE_CTL` just before `PLANE_SURF` (i915 order) so the surface write
    arms the whole plane state.
  - Detach both pipe-A scalers for the 1:1 native mode instead of leaving the
    firmware's upscaling window in place.
- `gfx.c`: hand the new framebuffer to the terminal with `gfx_attach_fb()`
  (allocate a backbuffer, update the terminal geometry) and return early instead
  of keeping the firmware aperture frame.
- `term.{c,h}`: add `term_update_size()` so the character grid follows the new
  framebuffer after the handoff.
- `fb.h` / `kmain.c`: drop the fixed `FB_WIDTH`/`FB_HEIGHT` limit; report the
  scanned geometry from the terminal framebuffer rather than the firmware
  framebuffer struct.
- `skl_display_dump()`: reduce to a concise key-register summary (clocks,
  pipe/plane geometry, eDP link/M/N, backlight). The large per-region scans and
  their helper were removed.

## Notes

- The mode set is display-only: it keeps the BIOS-trained eDP link and the
  display-core clocks (CDCLK/LCPLL), so no link training or CDCLK change is
  needed for a resolution change.
- The "content only in the top-left 800x600" image came from the pipe source
  and plane window being double-buffered and not armed, so the display kept the
  firmware's small boot region even though the registers read back the new size.
- The moving ripples came from overwriting the firmware's 18bpp data M/N and
  6bpc `PIPE_MISC` with 24bpp values.
- The scan-out framebuffer is a GTT buffer; it is handed to the console server
  after a successful mode set, otherwise the firmware aperture frame is kept.

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- QEMU is unchanged: the driver does not match QEMU's VGA device, so the mode
  set is never reached and the boot still reaches `name "hansh"`.
- Target Skylake/HD520 machine: the console fills the native 1366x768 panel
  with no ripple, and `skl_display_dump()` confirms the pipe, plane and eDP
  state; a step that fails falls back to the firmware framebuffer.
