#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <gpu/gfx.h>
#include <gpu/display_mode.h>

bool skl_edp_set_mode(gfx_pci_t *pci, gfx_mem_manager_t *mgr, gfx_gtt_t *gtt,
                      const display_mode_t *mode, gfx_fb_t *out_fb);
bool skl_cursor_init(gfx_pci_t *pci, gfx_mem_manager_t *mgr, gfx_gtt_t *gtt,
                     uint32_t width, uint32_t height);
void skl_cursor_move(int32_t dx, int32_t dy);
void skl_cursor_set(int32_t x, int32_t y);
bool skl_edid_read(gfx_pci_t *pci, uint8_t *buf, uint32_t len);
bool skl_panel_power_on(gfx_pci_t *pci);
void skl_display_dump(gfx_pci_t *pci);
