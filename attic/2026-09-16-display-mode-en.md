# 2026-09-16: Display Mode Model

## Overview

First step towards adaptive resolution: derive a complete display timing model
from the EDID preferred detailed timing (falling back to the current
framebuffer geometry) and log it. The display is not reprogrammed yet.

## Changes

- New `kernel/device/display/display_mode.{c,h}`: `display_mode_t` holds the
  pixel clock, horizontal/vertical active/blank/sync (offset and pulse), sync
  polarity, the interlaced flag, the scanline pitch and the refresh rate.
  `display_mode_from_edid()` decodes EDID detailed timing descriptor 1
  (including the MSB nibbles and the sync MSB bits), `display_mode_from_fb()`
  fills a geometry-only fallback, and `display_mode_log()` prints the result.
- `kernel/kmain.c`: after the EDID/framebuffer handling, build and log the
  boot display mode (EDID preferred timing, otherwise the framebuffer
  geometry).

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/display/display_mode.{c,h}` | display timing model |
| `kernel/kmain.c` | build and log the boot display mode |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free and the full image
  builds.
- QEMU decodes its EDID into `MODE: 1024x768 @ 74 Hz (source: EDID)`, pixel
  clock 82290 kHz, pitch 4096 bytes, with the H/V sync values decoded; the boot
  still reaches `name "hansh"` with no panic.
