/**-----------------------------------------------------------------------------

 @file    display_mode.c
 @brief   Display timing model derived from EDID or the current framebuffer
 @details
 @verbatim

  Keeps a copy of the mode the firmware booted with so other code can query
  the panel timing without re-parsing EDID.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <lib/klog.h>
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

static bool display_mode_from_dtd(const edid_info_t * edid, uint32_t index,
                                  display_mode_t * mode, bool preferred)
{
    __typeof__(edid->det_timings[0]) dtd = edid->det_timings[index];
    uint32_t hactive, hblank, vactive, vblank;
    uint32_t htotal, vtotal;
    uint32_t hsync_offset, hsync_pulse, vsync_offset, vsync_pulse;

    if (dtd.pixel_clock == 0)
        return false;

    hactive = dtd.horz_active
        | ((uint32_t) (dtd.horz_active_blank_msb & 0xF0) << 4);
    hblank = dtd.horz_bank
        | ((uint32_t) (dtd.horz_active_blank_msb & 0x0F) << 8);
    vactive = dtd.vert_active
        | ((uint32_t) (dtd.vert_active_blank_msb & 0xF0) << 4);
    vblank = dtd.vert_blank
        | ((uint32_t) (dtd.vert_active_blank_msb & 0x0F) << 8);

    hsync_offset = dtd.horz_sync_sffset
        | ((uint32_t) (dtd.sync_msb & 0xC0) << 2);
    hsync_pulse = dtd.horz_sync_pulse
        | ((uint32_t) (dtd.sync_msb & 0x30) << 4);
    vsync_offset = (dtd.vert_sync >> 4)
        | ((uint32_t) (dtd.sync_msb & 0x0C) << 2);
    vsync_pulse = (dtd.vert_sync & 0x0F)
        | ((uint32_t) (dtd.sync_msb & 0x03) << 4);

    htotal = hactive + hblank;
    vtotal = vactive + vblank;

    if (hactive == 0 || vactive == 0 || hblank == 0 || vblank == 0)
        return false;
    if (hactive > 4096 || vactive > 4096)
        return false;
    if (hsync_pulse == 0 && vsync_pulse == 0)
        return false;

    mode->valid = true;
    mode->from_edid = true;
    mode->preferred = preferred;

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
    mode->refresh_hz = display_mode_refresh(mode->pixel_clock_khz, htotal,
                                           vtotal);
    return true;
}

bool display_mode_from_edid(const edid_info_t * edid, display_mode_t * mode)
{
    display_mode_t best = { 0 };
    bool preferred;
    bool have = false;
    uint64_t best_pixels = 0;
    uint32_t best_refresh = 0;

    if (edid == NULL || mode == NULL)
        return false;

    /* The preferred-timing bit says DTD 1 (index 0) is authoritative. */
    preferred = (edid->dpms_flags & 0x02) != 0;
    if (preferred && display_mode_from_dtd(edid, 0, &best, true)) {
        *mode = best;
        boot_mode = *mode;
        return true;
    }

    /* Otherwise take the highest-resolution non-interlaced DTD, then the
     * highest refresh. Do not assume the native timing is DTD 0. */
    for (uint32_t i = 0; i < 4; i++) {
        display_mode_t candidate = { 0 };
        uint64_t pixels;

        if (!display_mode_from_dtd(edid, i, &candidate, false))
            continue;

        pixels = (uint64_t) candidate.hactive * candidate.vactive;
        if (!have || (!candidate.interlaced && best.interlaced)
            || (candidate.interlaced == best.interlaced
                && (pixels > best_pixels
                    || (pixels == best_pixels
                        && candidate.refresh_hz > best_refresh)))) {
            best = candidate;
            best_pixels = pixels;
            best_refresh = candidate.refresh_hz;
            have = true;
        }
    }

    if (!have)
        return false;

    *mode = best;
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
    mode->preferred = false;
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
