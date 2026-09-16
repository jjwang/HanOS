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
