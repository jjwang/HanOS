/**-----------------------------------------------------------------------------

 @file    skl_display.h
 @brief   Skylake Gen9 display modeset for the primary eDP port (DDI-A)

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <device/display/gfx.h>
#include <device/display/display_mode.h>

/* Program the primary eDP (DDI-A) display pipeline for `mode` and scan out a
 * GTT-allocated framebuffer. The firmware (GOP) has usually already powered
 * the display core and trained the eDP link, so this keeps those settings and
 * reprograms the transcoder timing, pipe and plane. Returns false (with the
 * reason logged) on the first step that times out. */
bool skl_edp_set_mode(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                      const display_mode_t * mode, gfx_fb_t * out_fb);

/* Log the current display pipeline registers (read-only). */
void skl_display_dump(gfx_pci_t * pci);

/* Program the 64x64 ARGB hardware cursor after a successful mode set. */
bool skl_cursor_init(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                     uint32_t width, uint32_t height);

/* Move the hardware cursor by a relative amount, clamped to the mode. */
void skl_cursor_move(int dx, int dy);

/* Place the hardware cursor at an absolute position, clamped to the mode. */
void skl_cursor_set(int x, int y);

/* Walk the cursor around the screen so it is visible without a pointer driver. */
void skl_cursor_selftest(void);
