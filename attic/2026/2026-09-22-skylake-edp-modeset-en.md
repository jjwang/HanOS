# 2026-09-22: Skylake eDP Mode Set

## Overview

Program the Skylake Gen9 display pipeline on the primary eDP port (DDI-A) at the boot mode. Hand the scanned framebuffer to the console server. Fix two issues on the target HD520 machine:

- A moving ripple on the panel.
- An image stuck at the firmware's 800x600 source region.

## Changes

- `gfx_reg.h`: add and correct the Gen9 mode-set registers: eDP transcoder timing, source copies and DP M/N; the eDP transcoder's shifted pipe config (`0x7F008`); the pipe-A scalers; `DC_STATE_EN`; the `DP_TP_CTL` link-train and enhanced-framing bits; the plane data-buffer and watermark fields; the AUX control fields.
- `skl_display.c`: tear the inherited pipeline down once in reverse order. Keep the firmware's eDP M/N and colour depth. Enable the transcoder/pipe in the documented order. Detach the firmware's panel scaler. Re-arm the double-buffered source/plane state at a vblank.
- `gfx.c`: hand the new framebuffer to the terminal with `gfx_attach_fb()` (allocate a backbuffer, update the terminal geometry). Return early instead of keeping the firmware aperture frame. Blank the frame the engine scans before the mode set.
- `term.{c,h}`: add `term_update_size()` so the character grid follows the new framebuffer after the handoff.
- `fb.c`: blit the scan-out with non-temporal stores. The kernel leaves no dirty cache lines for the console server.
- `fb.h` / `kmain.c`: drop the fixed `FB_WIDTH`/`FB_HEIGHT` limit. Report the scanned geometry from the terminal framebuffer, not the firmware framebuffer struct.
- `skl_display_dump()`: reduce to a concise key-register summary (clocks, pipe/plane geometry, eDP link/M/N, backlight). Remove the large per-region scans and their helper.

## Implementation

### Principles

- The display path is a chain (`plane -> pipe -> transcoder -> DDI -> PLL`). Enable it from the source outward and tear it down in reverse. Confirm the pipe stopped, its frame counter no longer advancing, before touching the clock tree. Otherwise a scanning pipe runs on a clock that changes under it.
- The resolution and the link training are independent. The resolution sets the pipe/transcoder timings (the pixel clock). Link training sets the symbol clock / link rate. Changing only the resolution needs no retrain, the same choice i915 makes for a fastset. A retrain is required only when the link rate or lane count changes.
- Source size and plane window are double-buffered. They take effect only after their state is armed at a vblank. Re-write them after the pipe runs.
- The panel runs at 6bpc. The firmware's data M/N and colour depth pace the link correctly and must be preserved. Recomputing them for 24bpp desynchronises the sink.
- The engine reads DRAM. Arrange the CPU-side writers so no cached copy is left to be written back over the scan-out later.

### Operations

At takeover, once, from the firmware frame:

1. Disable the old pipeline in reverse order: plane, pipe, transcoder, DDI. Confirm the pipe stopped before touching any clock.
2. Configure clocks and link only as far as the change needs: CDCLK, then the PLL for the new link rate, then `DDI_BUF_TRANS`, then AUX training (CR -> CE). A resolution-only mode set needs none of this, so keep the firmware's CDCLK, PLL and training.
3. Enable the new pipeline in forward order: `DDI_BUF_CTL`, transcoder, pipe, plane. Re-arm the double-buffered source and plane state at a vblank.
4. Hand the scanned framebuffer to the console. Stop the kernel's own terminal writes to the scan-out.

The subsections below give the register-level detail for each part.

### Framebuffer allocation and geometry

- `skl_edp_set_mode()` sizes the scan-out itself, not the boot framebuffer.
- It computes `pitch = (hactive * 4 + 63) & ~63`. 1366 px → 5464 → 5504, so each row starts on a 64-byte boundary as `PLANE_STRIDE` requires.
- It computes `fbsize = pitch * vactive`.
- `gfx_alloc()` takes `ceil(fbsize / 4096)` contiguous physical pages from `pmm_get`.
- It aligns the shared GPU address to the requested alignment.
- It writes one GTT entry per page through `gfx_gtt_map()`; each 4 KiB GPU page points at the matching physical page.
- It returns the GPU address used by the plane and the CPU address (`PHYS_TO_VIRT(phys)`) used by the kernel.
- The mode set clears the new buffer through the cacheable direct map.
- It flushes the buffer with `wbinvd` before the plane points at it. The engine never reads stale DRAM.

### Inherited state and teardown

`modeset_save()` snapshots every register the mode set can touch.

- It covers the six transcoder timing registers and `PIPEACONF`.
- It covers the pipe/eDP source, shifted-pipe config, eDP M/N and `PIPE_MISC`.
- It covers `TRANS_DDI_FUNC_CTL`, `DP_TP_CTL` and both scalers.
- It captures the plane control/stride/surf/offset/pos/size, backlight and watermarks.

`modeset_restore()` puts the firmware state back if a later step fails. Teardown is the reverse of the enable order: plane, pipe, transcoder, DDI, PLL. `wait_pipe_off()` first waits for both `PIPE_STATE` bits to clear. Then it requires the frame counter at `0x70040` to stop advancing across two consecutive 40 ms samples. The state bits read back zero even while the firmware still scans, so the counter is the direct evidence. A failed confirmation aborts the mode set before the clock tree is touched with a live pipe.

### Enabling the pipeline

The enable steps, in order:

1. Write source size on the pipe and both eDP copies. `PIPEASRC`, `TRANS_EDP_SRC` and `TRANS_EDP_PIPE_SRC` all take `((hactive-1) << 16) | (vactive-1)`.
2. Write transcoder timing on transcoder 0 (`0x60000`) and on the eDP transcoder (`0x6F000`, transcoder 15). `transcoder_timing()` writes the low 16 bits of each register with the first edge (active/start) and the high 16 bits with the second (total/end), matching i915: `HTOTAL`/`HBLANK`, `HSYNC`, `VTOTAL`/ `VBLANK`, `VSYNC` all derive from the mode's active/blank/sync fields.
3. Write back the firmware's `TRANS_EDP_DATA_M1/N1`, `TRANS_EDP_LINK_M1/N1` and `PIPE_MISC` verbatim for DP M/N and colour depth. Write `LINK_N1` last because it arms the double-buffered M/N update.
4. Re-enable `DP_TP_CTL` with the sink's negotiated enhanced-framing bit and normal link training. Enable `DDI_BUF_CTL_A` (port width x1), then `TRANS_DDI_FUNC_CTL_EDP`, then set the eDP MSA misc register `0x6F410` to `0x01` (8bpc, sync clock).
5. Write the eDP transcoder's own shifted-pipe config `TRANS_EDP_PIPE_CONF` (`0x7F008`) disabled first, then enabled with `PIPE_ENABLE`, so the state machine transitions cleanly. Enable `PIPEACONF` the same way. Poll `PIPE_STATE` (bounded) to confirm the pipe started.
6. Clear both pipe-A scalers (`PS_CTRL`/`PS_WIN_POS`/`PS_WIN_SZ` for pipe 1 and 2). The firmware had left one upscaling its 800x600 boot mode; it would otherwise scale the native source a second time.
7. Expand the plane data buffer to the whole display buffer (`PLANE_BUF_END(445) | PLANE_BUF_START(0)`). Set all eight watermarks plus the transition watermark to `PLANE_WM_EN | IGNORE_LINES | BLOCKS(32)` so the full-resolution fetch cannot starve.

### Plane programming

`plane_configure()` disables the plane and zeroes offset/pos. It writes `PLANE_SIZE = ((vactive-1) << 16) | (hactive-1)` and `PLANE_STRIDE = pitch / 64`. Then it writes `PLANE_CTL` with `ENABLE | FORMAT_XRGB8888 | TILED_LINEAR`, and finally `PLANE_SURF` with the GPU address. The control register is written before the surface because the surface write arms the whole double-buffered plane state at the next vblank (i915 `skl_plane_update_arm`). Once the pipe and plane run, the source size on the pipe, both eDP copies and `PLANE_SIZE`/`PLANE_SURF` are written a second time at a vblank. Enabling the eDP pipe resets the source back to the firmware's small boot region, so the pre-enable write does not stick.

### Console handoff and scan-out coherence

`gfx_attach_fb()` points the terminal at the GTT object (`addr`, `width`, `height`, `pitch`). It allocates a private zeroed backbuffer of `stride * height` bytes and calls `term_update_size()` to recompute the character grid from the new geometry. The console server then sizes its mapping as `pitch * height`, maps it write-combining into the server's address space and claims the screen before the server spawns, so no stale kernel refresh can overwrite its first frames. The kernel reaches the scan-out through its cacheable direct map; the server maps it write-combining. `fb_refresh()` therefore writes the scan-out with non-temporal stores (`movnti` + `sfence`). A non-temporal store never allocates a cache line, so no dirty line can be evicted later and write back over the server's frames.

### Blanking before the mode set

Before `skl_edp_set_mode()` runs, `gfx_init()` clears the framebuffer the engine currently scans to black and refreshes it for 50 ms. A panel that keeps sending the last region it received until the new pipe covers it then retains an empty frame, instead of firmware content.

## Notes

- The mode set is display-only. It keeps the BIOS-trained eDP link and the display-core clocks (CDCLK/LCPLL), so a resolution change needs no link training or CDCLK change.
- The "content only in the top-left 800x600" image came from the double-buffered pipe source and plane window being unarmed. The display kept the firmware's small boot region even though the registers read back the new size.
- The moving ripples came from overwriting the firmware's 18bpp data M/N and 6bpc `PIPE_MISC` with 24bpp values.
- The scan-out framebuffer is a GTT buffer. It is handed to the console server after a successful mode set; otherwise the firmware aperture frame is kept.

## Testing

- `make -C kernel clean && make -C kernel` builds with no warnings and the full image builds.
- QEMU is unchanged. The driver does not match QEMU's VGA device, so the mode set is never reached and the boot still reaches `name "hansh"`.
- Target Skylake/HD520 machine: the console fills the native 1366x768 panel with no ripple, and `skl_display_dump()` confirms the pipe, plane and eDP state. A failed step falls back to the firmware framebuffer.
