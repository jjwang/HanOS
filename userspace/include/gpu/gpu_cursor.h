#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <gpu/gfx.h>

bool skl_cursor_init(gfx_pci_t *pci, gfx_mem_manager_t *mgr, gfx_gtt_t *gtt,
                     uint32_t width, uint32_t height);
void skl_cursor_move(int32_t dx, int32_t dy);
void skl_cursor_set(int32_t x, int32_t y);
