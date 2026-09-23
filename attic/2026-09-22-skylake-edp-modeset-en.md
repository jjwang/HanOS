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
- `skl_display.c`: tear the inherited pipeline down once in reverse order, keep
  the firmware's eDP M/N and colour depth, enable the transcoder/pipe in the
  documented order, detach the firmware's panel scaler, and re-arm the
  double-buffered source/plane state at a vblank.
- `gfx.c`: hand the new framebuffer to the terminal with `gfx_attach_fb()`
  (allocate a backbuffer, update the terminal geometry) and return early instead
  of keeping the firmware aperture frame; blank the frame the engine is
  currently scanning before the mode set.
- `term.{c,h}`: add `term_update_size()` so the character grid follows the new
  framebuffer after the handoff.
- `fb.c`: blit the scan-out with non-temporal stores so the kernel leaves no
  dirty cache lines behind for the console server.
- `fb.h` / `kmain.c`: drop the fixed `FB_WIDTH`/`FB_HEIGHT` limit; report the
  scanned geometry from the terminal framebuffer rather than the firmware
  framebuffer struct.
- `skl_display_dump()`: reduce to a concise key-register summary (clocks,
  pipe/plane geometry, eDP link/M/N, backlight). The large per-region scans and
  their helper were removed.

## Implementation

### Framebuffer allocation and geometry

`skl_edp_set_mode()` sizes the scan-out itself rather than trusting the boot
framebuffer: `pitch = (hactive * 4 + 63) & ~63` (1366 px → 5464 → 5504, so each
row starts on a 64-byte boundary as `PLANE_STRIDE` requires) and
`fbsize = pitch * vactive`. `gfx_alloc()` then takes `ceil(fbsize / 4096)`
contiguous physical pages from `pmm_get`, aligns the shared GPU address to the
requested alignment, and writes one GTT entry per page through `gfx_gtt_map()` —
each 4 KiB GPU page pointing at the matching physical page. It returns the GPU
address used by the plane and the CPU address (`PHYS_TO_VIRT(phys)`) used by the
kernel. The mode set clears the new buffer through the cacheable direct map and
flushes it with `wbinvd` before the plane points at it, so the engine never
reads stale DRAM.

### Inherited state and teardown

`modeset_save()` snapshots every register the mode set can touch — the six
transcoder timing registers, `PIPEACONF`, the pipe/eDP source and shifted-pipe
config, the eDP M/N, `PIPE_MISC`, `TRANS_DDI_FUNC_CTL`, `DP_TP_CTL`, both
scalers, the plane control/stride/surf/offset/pos/size, the backlight and the
watermarks — so `modeset_restore()` can put the firmware state back if a later
step fails. Teardown is the reverse of the enable order: plane, pipe, transcoder,
DDI, PLL. `wait_pipe_off()` first waits for both `PIPE_STATE` bits to clear and
then insists that the frame counter at `0x70040` stops advancing across two
consecutive 40 ms samples: the state bits read back zero even while the firmware
is still scanning, so the counter is the direct evidence. A failed confirmation
aborts the mode set before the clock tree is touched with a live pipe.

### Enabling the pipeline

The enable sequence follows the order in `command.txt`:

1. Source size on the pipe and both eDP copies — `PIPEASRC`, `TRANS_EDP_SRC`
   and `TRANS_EDP_PIPE_SRC` are all written `((hactive-1) << 16) | (vactive-1)`.
2. Transcoder timing on transcoder 0 (`0x60000`) and on the eDP transcoder
   (`0x6F000`, transcoder 15). `transcoder_timing()` writes the low 16 bits of
   each register with the first edge (active/start) and the high 16 bits with the
   second (total/end), matching i915: `HTOTAL`/`HBLANK`, `HSYNC`, `VTOTAL`/
   `VBLANK`, `VSYNC` all derived from the mode's active/blank/sync fields.
3. DP M/N and colour depth: the firmware's `TRANS_EDP_DATA_M1/N1`,
   `TRANS_EDP_LINK_M1/N1` and `PIPE_MISC` are written back verbatim. `LINK_N1` is
   written last because it arms the double-buffered M/N update.
4. `DP_TP_CTL` is re-enabled with the sink's negotiated enhanced-framing bit and
   normal link training, then `DDI_BUF_CTL_A` (port width x1) is enabled, then
   `TRANS_DDI_FUNC_CTL_EDP`, then the eDP MSA misc register `0x6F410` is set to
   `0x01` (8bpc, sync clock).
5. The eDP transcoder's own shifted-pipe config `TRANS_EDP_PIPE_CONF` (`0x7F008`)
   is written disabled first and then enabled with `PIPE_ENABLE`, so the state
   machine transitions cleanly; `PIPEACONF` is enabled the same way and the code
   polls `PIPE_STATE` (bounded) to confirm the pipe started.
6. Both pipe-A scalers (`PS_CTRL`/`PS_WIN_POS`/`PS_WIN_SZ` for pipe 1 and 2) are
   cleared, because the firmware had left one upscaling its 800x600 boot mode and
   it would otherwise scale the native source a second time.
7. The plane data buffer is expanded to the whole display buffer
   (`PLANE_BUF_END(445) | PLANE_BUF_START(0)`) and all eight watermarks plus the
   transition watermark are set to `PLANE_WM_EN | IGNORE_LINES | BLOCKS(32)` so
   the full-resolution fetch cannot starve.

### Plane programming

`plane_configure()` disables the plane, zeroes offset/pos, writes
`PLANE_SIZE = ((vactive-1) << 16) | (hactive-1)` and
`PLANE_STRIDE = pitch / 64`, then writes `PLANE_CTL` with
`ENABLE | FORMAT_XRGB8888 | TILED_LINEAR`, and finally `PLANE_SURF` with the GPU
address. The control register is written before the surface because the surface
write arms the whole double-buffered plane state at the next vblank (i915
`skl_plane_update_arm`). Once the pipe and plane are running, the source size on
the pipe, both eDP copies and `PLANE_SIZE`/`PLANE_SURF` are written a second time
at a vblank: enabling the eDP pipe resets the source back to the firmware's small
boot region, so the pre-enable write does not stick.

### Console handoff and scan-out coherence

`gfx_attach_fb()` points the terminal at the GTT object (`addr`, `width`,
`height`, `pitch`), allocates a private zeroed backbuffer of `stride * height`
bytes, and calls `term_update_size()` to recompute the character grid from the
new geometry. The console server then sizes its mapping as `pitch * height`,
maps it write-combining into the server's address space and claims the screen
before the server is spawned, so no stale kernel refresh can overwrite its first
frames. Because the kernel reaches the scan-out through its cacheable direct map
while the server maps it write-combining, `fb_refresh()` writes the scan-out with
non-temporal stores (`movnti` + `sfence`): a non-temporal store never allocates a
cache line, so no dirty line can be evicted later and write back over the
server's frames.

### Blanking before the mode set

Before `skl_edp_set_mode()` runs, `gfx_init()` clears the framebuffer the engine
is currently scanning to black and refreshes it for 50 ms. A panel that keeps
sending the last region it received until the new pipe covers it then has an
empty frame to retain, instead of firmware content.

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
