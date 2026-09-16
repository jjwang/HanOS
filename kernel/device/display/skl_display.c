/**-----------------------------------------------------------------------------

 @file    skl_display.c
 @brief   Skylake Gen9 display modeset for the primary eDP port (DDI-A)

   The mode set keeps the firmware's display-core power, CDCLK, DPLL0 and
   trained eDP link (the GOP already established them to show the boot
   framebuffer) and reprograms the transcoder timing, pipe and plane to scan
   out a GTT-mapped framebuffer at the requested mode. Every step that waits on
   hardware has a timeout so a failure can fall back to the firmware frame.

 **-----------------------------------------------------------------------------
 */
#include <base/klog.h>
#include <sys/cpu.h>
#include <sys/pci.h>
#include <sys/pit.h>
#include <device/display/gfx_reg.h>
#include <device/display/skl_display.h>

#define MODESET_TIMEOUT_MS  300

/* Wait until (reg & mask) == want, or time out. */
static bool wait_bits(gfx_pci_t * pci, uint32_t reg, uint32_t mask,
                      uint32_t want, const char *what)
{
    for (uint32_t i = 0; i < MODESET_TIMEOUT_MS; i++) {
        if ((gfx_ind(pci, reg) & mask) == want)
            return true;
        pit_wait(1);
    }

    kloge("GFX: modeset: timeout waiting for %s (0x%05x=0x%08x)\n",
          what, reg, gfx_ind(pci, reg));
    return false;
}

/* Enable a power well if it is not already on (req bit = write, state = read). */
static bool power_well_on(gfx_pci_t * pci, uint32_t ctl, uint32_t req,
                          uint32_t state, const char *name)
{
    uint32_t v = gfx_ind(pci, ctl);
    if ((v & state) == state) {
        klogd("GFX: modeset: %s well already on\n", name);
        return true;
    }

    gfx_outd(pci, ctl, v | req);
    if (!wait_bits(pci, ctl, state, state, name))
        return false;
    klogi("GFX: modeset: %s well enabled\n", name);
    return true;
}

static bool panel_power_on(gfx_pci_t * pci)
{
    uint32_t pp = gfx_ind(pci, PP_CONTROL);
    if (!(pp & PP_CONTROL_POWER_STATE)) {
        gfx_outd(pci, PP_CONTROL, pp | PP_CONTROL_POWER_STATE
                 | PP_CONTROL_VDD_FORCE);
    }
    return wait_bits(pci, PP_STATUS, PP_STATUS_ON, PP_STATUS_ON,
                     "eDP panel power");
}

static void backlight_on(gfx_pci_t * pci, uint32_t level)
{
    gfx_outd(pci, BLC_PWM_CTL, level << 16);
    gfx_outd(pci, BLC_PWM_CTL2, BLC_PWM_CTL_ENABLE);
    klogi("GFX: modeset: backlight on (duty %u)\n", level);
}

/* Program transcoder timing registers for one pipe/transcoder. */
static void transcoder_timing(gfx_pci_t * pci, uint8_t tr,
                              const display_mode_t * m)
{
    uint32_t base = 0x60000 + (uint32_t) tr * 0x1000;

    uint32_t htotal = m->hactive + m->hblank;
    uint32_t vtotal = m->vactive + m->vblank;
    uint32_t hsync_start = m->hactive + m->hsync_offset;
    uint32_t hsync_end = hsync_start + m->hsync_pulse;
    uint32_t vsync_start = m->vactive + m->vsync_offset;
    uint32_t vsync_end = vsync_start + m->vsync_pulse;

    gfx_outd(pci, base + 0x00, (htotal - 1) | ((m->hactive - 1) << 16));
    gfx_outd(pci, base + 0x04, (htotal - 1) | ((hsync_start - 1) << 16));
    gfx_outd(pci, base + 0x08,
             (hsync_end - 1) | ((hsync_start - 1) << 16)
             | (m->hsync_positive ? 0 : (1u << 31)));
    gfx_outd(pci, base + 0x0C, (vtotal - 1) | ((m->vactive - 1) << 16));
    gfx_outd(pci, base + 0x10, (vtotal - 1) | ((vsync_start - 1) << 16));
    gfx_outd(pci, base + 0x14,
             (vsync_end - 1) | ((vsync_start - 1) << 16)
             | (m->vsync_positive ? 0 : (1u << 31)));
}

/* Enable the transcoder's DDI output (eDP on DDI-A, DP SST, 8bpc). The port
 * width is taken from the link the firmware trained. */
static void transcoder_ddi_enable(gfx_pci_t * pci, const display_mode_t * m)
{
    uint32_t width = gfx_ind(pci, DDI_BUF_CTL_A) & DDI_BUF_CTL_PORT_WIDTH_MASK;

    gfx_outd(pci, TRANS_DDI_FUNC_CTL_A,
             TRANS_DDI_FUNC_ENABLE | TRANS_DDI_SELECT_DDI_A
             | TRANS_DDI_MODE_DP_SST | TRANS_DDI_BPC_8 | width);
    gfx_outd(pci, TRANS_DP_CTL_A,
             TRANS_DP_OUTPUT_ENABLE | TRANS_DP_BPC_8
             | (m->vsync_positive ? TRANS_DP_VSYNC_ACTIVE_HIGH : 0)
             | (m->hsync_positive ? TRANS_DP_HSYNC_ACTIVE_HIGH : 0));
    (void) gfx_ind(pci, TRANS_DP_CTL_A);
}

/* Configure pipe A for progressive scan at 8bpc and the given source size. */
static bool pipe_configure(gfx_pci_t * pci, const display_mode_t * m)
{
    gfx_outd(pci, PIPEACONF, 0);
    if (!wait_bits(pci, PIPEACONF, PIPE_STATE, 0, "pipe A disable"))
        return false;

    gfx_outd(pci, PIPEACONF, PIPE_PROGRESSIVE);
    gfx_outd(pci, PIPE_MISC_A, PIPE_MISC_BPC_8);
    gfx_outd(pci, PIPEASRC,
             (m->vactive - 1) << 16 | (m->hactive - 1));

    gfx_outd(pci, PIPEACONF, PIPE_ENABLE | PIPE_PROGRESSIVE);
    return wait_bits(pci, PIPEACONF, PIPE_STATE, PIPE_STATE, "pipe A enable");
}

/* Point plane 1 of pipe A at the GTT-mapped framebuffer. */
static void plane_configure(gfx_pci_t * pci, const display_mode_t * m,
                            uint64_t gfx_addr, uint32_t pitch)
{
    gfx_outd(pci, PLANE_CTL_1_A, 0);
    gfx_outd(pci, PLANE_OFFSET_1_A, 0);
    gfx_outd(pci, PLANE_POS_1_A, 0);
    gfx_outd(pci, PLANE_SIZE_1_A,
             (m->vactive - 1) << 16 | (m->hactive - 1));
    gfx_outd(pci, PLANE_STRIDE_1_A, pitch / 64);
    gfx_outd(pci, PLANE_SURF_1_A, (uint32_t) gfx_addr);
    gfx_outd(pci, PLANE_CTL_1_A,
             PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB8888
             | PLANE_CTL_TILED_LINEAR);
    (void) gfx_ind(pci, PLANE_SURF_1_A);
}

bool skl_edp_set_mode(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                      const display_mode_t * mode, gfx_fb_t * out_fb)
{
    if (mode == NULL || !mode->valid || mode->hactive == 0
        || mode->vactive == 0) {
        kloge("GFX: modeset: invalid mode\n");
        return false;
    }

    uint32_t pitch = (mode->hactive * 4 + 63) & ~63u;
    uint64_t fbsize = (uint64_t) pitch * mode->vactive;

    gfx_object_t obj = { 0 };
    if (!gfx_alloc(mgr, gtt, &obj, fbsize, 64)) {
        kloge("GFX: modeset: framebuffer allocation (%lu bytes) failed\n",
              fbsize);
        return false;
    }
    /* Test pattern: a grey field so a successful mode set is obvious. */
    for (uint64_t i = 0; i < fbsize / 4; i++)
        ((volatile uint32_t *) obj.cpu_addr)[i] = 0x00404040;

    klogi("GFX: modeset: fb 0x%016lx (%ux%u pitch %u)\n",
          obj.gfx_addr, mode->hactive, mode->vactive, pitch);

    /* Keep the firmware's display core up; only turn it on if it is off. */
    if (!power_well_on(pci, PWR_WELL_CTL1, PWR_WELL_CTL1_DC_REQ,
                       PWR_WELL_CTL1_DC_STATE, "DC"))
        return false;
    if (!power_well_on(pci, PWR_WELL_CTL2, PWR_WELL_DDI_A_REQ,
                       PWR_WELL_DDI_A_STATE, "DDI-A"))
        return false;

    /* The GOP trained the eDP link and enabled DPLL0; require that. */
    if (!(gfx_ind(pci, DPLL_STATUS) & DPLL0_LOCK)) {
        kloge("GFX: modeset: DPLL0 not locked (status 0x%08x)\n",
              gfx_ind(pci, DPLL_STATUS));
        return false;
    }

    if (!panel_power_on(pci))
        return false;

    /* Program the output pipe: transcoder timing, pipe and plane. */
    transcoder_timing(pci, 0, mode);
    if (!pipe_configure(pci, mode))
        return false;
    transcoder_ddi_enable(pci, mode);
    plane_configure(pci, mode, obj.gfx_addr, pitch);
    backlight_on(pci, 0xFFFF);

    out_fb->obj = obj;
    out_fb->width = mode->hactive;
    out_fb->height = mode->vactive;
    out_fb->stride = pitch;
    out_fb->format = DISPPLANE_BGRX888;

    klogi("GFX: modeset: %ux%u @ %u Hz done\n", mode->hactive, mode->vactive,
          mode->refresh_hz);
    return true;
}

void skl_display_dump(gfx_pci_t * pci)
{
    klogi("GFX: display state:\n");
    klogi("  PWR_WELL1 0x%08x PWR_WELL2 0x%08x\n",
          gfx_ind(pci, PWR_WELL_CTL1), gfx_ind(pci, PWR_WELL_CTL2));
    klogi("  CDCLK 0x%08x LCPLL1 0x%08x DPLL_STATUS 0x%08x\n",
          gfx_ind(pci, CDCLK_CTL), gfx_ind(pci, LCPLL1_CTL),
          gfx_ind(pci, DPLL_STATUS));
    klogi("  PIPEACONF 0x%08x PIPEASRC 0x%08x\n", gfx_ind(pci, PIPEACONF),
          gfx_ind(pci, PIPEASRC));
    klogi("  TRANS_DDI 0x%08x PLANE_CTL 0x%08x PLANE_SURF 0x%08x\n",
          gfx_ind(pci, TRANS_DDI_FUNC_CTL_A), gfx_ind(pci, PLANE_CTL_1_A),
          gfx_ind(pci, PLANE_SURF_1_A));
    klogi("  PP_STATUS 0x%08x PP_CONTROL 0x%08x\n", gfx_ind(pci, PP_STATUS),
          gfx_ind(pci, PP_CONTROL));
    klogi("  DDI_BUF_CTL_A 0x%08x DDI_AUX_CTL_A 0x%08x\n",
          gfx_ind(pci, DDI_BUF_CTL_A), gfx_ind(pci, DDI_AUX_CTL_A));
}
