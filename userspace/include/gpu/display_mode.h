#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool valid;
    bool from_edid;
    bool preferred;

    uint32_t pixel_clock_khz;
    uint32_t hactive;
    uint32_t hblank;
    uint32_t hsync_offset;
    uint32_t hsync_pulse;
    uint32_t vactive;
    uint32_t vblank;
    uint32_t vsync_offset;
    uint32_t vsync_pulse;
    bool hsync_positive;
    bool vsync_positive;
    bool interlaced;
    uint32_t pitch;
    uint32_t refresh_hz;
} display_mode_t;
