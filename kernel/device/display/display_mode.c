/**-----------------------------------------------------------------------------

 @file    display_mode.c
 @brief   Display timing model derived from EDID or the current framebuffer

 **-----------------------------------------------------------------------------
 */
#include <base/klog.h>
#include <device/display/display_mode.h>

static display_mode_t boot_mode = { 0 };

const display_mode_t *display_mode_get_boot(void)
{
    return &boot_mode;
}

/* Refresh rate in Hz from a pixel clock in kHz and the total H/V counts. */
static uint32_t display_mode_refresh(uint32_t pixel_clock_khz, uint32_t htotal,
                                     uint32_t vtotal)
{
    if (pixel_clock_khz == 0 || htotal == 0 || vtotal == 0)
        return 0;

    uint64_t denom = (uint64_t) htotal * (uint64_t) vtotal;
    return (uint32_t) (((uint64_t) pixel_clock_khz * 1000) / denom);
}

bool display_mode_from_edid(const edid_info_t * edid, display_mode_t * mode)
{
    if (edid == NULL || mode == NULL)
        return false;

    /* Copy out of the packed EDID so field access is not unaligned. */
    __typeof__(edid->det_timings[0]) dtd = edid->det_timings[0];

    /* A zero pixel clock means the descriptor is not a detailed timing. */
    if (dtd.pixel_clock == 0)
        return false;

    uint32_t hactive = dtd.horz_active
        | ((uint32_t) (dtd.horz_active_blank_msb & 0xF0) << 4);
    uint32_t hblank = dtd.horz_bank
        | ((uint32_t) (dtd.horz_active_blank_msb & 0x0F) << 8);
    uint32_t vactive = dtd.vert_active
        | ((uint32_t) (dtd.vert_active_blank_msb & 0xF0) << 4);
    uint32_t vblank = dtd.vert_blank
        | ((uint32_t) (dtd.vert_active_blank_msb & 0x0F) << 8);

    uint32_t hsync_offset = dtd.horz_sync_sffset
        | ((uint32_t) (dtd.sync_msb & 0xC0) << 2);
    uint32_t hsync_pulse = dtd.horz_sync_pulse
        | ((uint32_t) (dtd.sync_msb & 0x30) << 4);
    uint32_t vsync_offset = (dtd.vert_sync >> 4)
        | ((uint32_t) (dtd.sync_msb & 0x0C) << 2);
    uint32_t vsync_pulse = (dtd.vert_sync & 0x0F)
        | ((uint32_t) (dtd.sync_msb & 0x03) << 4);

    mode->valid = true;
    mode->from_edid = true;

    mode->pixel_clock_khz = (uint32_t) dtd.pixel_clock * 10;

    mode->hactive = hactive;
    mode->hblank = hblank;
    mode->hsync_offset = hsync_offset;
    mode->hsync_pulse = hsync_pulse;

    mode->vactive = vactive;
    mode->vblank = vblank;
    mode->vsync_offset = vsync_offset;
    mode->vsync_pulse = vsync_pulse;

    mode->interlaced = (dtd.features & 0x80) != 0;
    mode->vsync_positive = (dtd.features & 0x04) != 0;
    mode->hsync_positive = (dtd.features & 0x02) != 0;

    mode->pitch = hactive * (DISPLAY_MODE_BPP / 8);
    mode->refresh_hz = display_mode_refresh(mode->pixel_clock_khz,
                                           hactive + hblank, vactive + vblank);
    boot_mode = *mode;
    return true;
}

void display_mode_from_fb(display_mode_t * mode, uint32_t width,
                          uint32_t height, uint32_t pitch)
{
    if (mode == NULL)
        return;

    mode->valid = (width != 0 && height != 0);
    mode->from_edid = false;
    mode->pixel_clock_khz = 0;

    mode->hactive = width;
    mode->hblank = 0;
    mode->hsync_offset = 0;
    mode->hsync_pulse = 0;

    mode->vactive = height;
    mode->vblank = 0;
    mode->vsync_offset = 0;
    mode->vsync_pulse = 0;

    mode->hsync_positive = false;
    mode->vsync_positive = false;
    mode->interlaced = false;

    mode->pitch = (pitch != 0) ? pitch : width * (DISPLAY_MODE_BPP / 8);
    mode->refresh_hz = 0;
    boot_mode = *mode;
}

void display_mode_log(const display_mode_t * mode)
{
    if (mode == NULL || !mode->valid) {
        klogi("MODE: no usable display mode\n");
        return;
    }

    klogi("MODE: %ux%u @ %u Hz%s (source: %s)\n",
          mode->hactive, mode->vactive, mode->refresh_hz,
          mode->interlaced ? " interlaced" : "",
          mode->from_edid ? "EDID" : "framebuffer");
    klogi("  pixel clock %u kHz, pitch %u bytes\n",
          mode->pixel_clock_khz, mode->pitch);
    klogi("  H: active %u blank %u sync %u+%u (%s)\n",
          mode->hactive, mode->hblank, mode->hsync_offset, mode->hsync_pulse,
          mode->hsync_positive ? "pos" : "neg");
    klogi("  V: active %u blank %u sync %u+%u (%s)\n",
          mode->vactive, mode->vblank, mode->vsync_offset, mode->vsync_pulse,
          mode->vsync_positive ? "pos" : "neg");
}
