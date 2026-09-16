/**-----------------------------------------------------------------------------

 @file    display_mode.h
 @brief   Display timing model derived from EDID or the current framebuffer

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <device/display/edid.h>

/* Scanout pixel format assumed for the pitch computation. */
#define DISPLAY_MODE_BPP    32

typedef struct {
    bool valid;
    bool from_edid;

    uint32_t pixel_clock_khz;

    uint32_t hactive;
    uint32_t hblank;
    uint32_t hsync_offset;      /* horizontal front porch */
    uint32_t hsync_pulse;

    uint32_t vactive;
    uint32_t vblank;
    uint32_t vsync_offset;      /* vertical front porch */
    uint32_t vsync_pulse;

    bool hsync_positive;
    bool vsync_positive;
    bool interlaced;

    uint32_t pitch;             /* bytes per scanline */
    uint32_t refresh_hz;
} display_mode_t;

/* Build a mode from the EDID preferred detailed timing (descriptor 1).
 * Returns false when the descriptor carries no timing (pixel clock is zero). */
bool display_mode_from_edid(const edid_info_t * edid, display_mode_t * mode);

/* Build a geometry-only mode from the current framebuffer, used when no EDID
 * detailed timing is available. */
void display_mode_from_fb(display_mode_t * mode, uint32_t width,
                          uint32_t height, uint32_t pitch);

/* Log a mode's fields. No-op when mode is NULL or invalid. */
void display_mode_log(const display_mode_t * mode);

/* The mode selected at boot (EDID preferred, else framebuffer geometry). */
const display_mode_t *display_mode_get_boot(void);

