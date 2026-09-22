/**-----------------------------------------------------------------------------

 @file    skl_display.c
 @brief   Skylake Gen9 display modeset for the primary eDP port (DDI-A)

   The firmware boots the panel through the eDP transcoder and already
   programmed it for the native mode; DPLL0 and the eDP link are likewise
   established. The mode set therefore keeps that timing and clock and only
   points pipe A's plane at a GTT-mapped framebuffer, so no part of the live
   transport is disturbed. Steps that wait on hardware have a timeout so a
   failure can fall back to the firmware frame.

 **-----------------------------------------------------------------------------
 */
#include <base/klog.h>
#include <libc/string.h>
#include <sys/cpu.h>
#include <sys/pci.h>
#include <sys/pit.h>
#include <device/display/gfx_reg.h>
#include <device/display/skl_display.h>

#define MODESET_TIMEOUT_MS  300

static bool aux_dpcd_write(gfx_pci_t * pci, uint32_t addr, const uint8_t * buf,
                           uint32_t len);
static bool aux_dpcd_read(gfx_pci_t * pci, uint32_t addr, uint8_t * buf,
                          uint32_t len);

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

/* True while the pipe's frame counter keeps advancing (the pipe is scanning). */
static bool pipe_is_running(gfx_pci_t * pci)
{
    uint32_t a = gfx_ind(pci, 0x70040);
    pit_wait(40);
    return gfx_ind(pci, 0x70040) != a;
}

/* Wait for the next vblank (frame counter advance), bounded. */
static void wait_vblank(gfx_pci_t * pci)
{
    uint32_t a = gfx_ind(pci, 0x70040);
    for (int i = 0; i < 40 && gfx_ind(pci, 0x70040) == a; i++)
        pit_wait(1);
}

/* Confirm the pipe has really stopped scanning. The pipe state bits are the
 * fast path, but they read back 0 even while the firmware scans, so the direct
 * evidence is the frame counter: it must not advance over more than a frame. */
static bool wait_pipe_off(gfx_pci_t * pci)
{
    for (uint32_t i = 0; i < MODESET_TIMEOUT_MS; i++) {
        if (!(gfx_ind(pci, TRANS_EDP_PIPE_CONF) & PIPE_STATE)
            && !(gfx_ind(pci, PIPEACONF) & PIPE_STATE))
            break;
        pit_wait(1);
    }

    for (uint32_t i = 0; i < 10; i++) {
        uint32_t a = gfx_ind(pci, 0x70040);

        pit_wait(40);
        if (gfx_ind(pci, 0x70040) == a) {
            klogi("GFX: modeset: pipe off confirmed (PIPEACONF 0x%08x eDP "
                  "conf 0x%08x FRM %u)\n", gfx_ind(pci, PIPEACONF),
                  gfx_ind(pci, TRANS_EDP_PIPE_CONF), a);
            return true;
        }
    }

    kloge("GFX: modeset: pipe still scanning after disable (PIPEACONF "
          "0x%08x eDP conf 0x%08x FRM %u)\n", gfx_ind(pci, PIPEACONF),
          gfx_ind(pci, TRANS_EDP_PIPE_CONF), gfx_ind(pci, 0x70040));
    return false;
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

    /* The low 16 bits of each register hold the first edge (active/start) and
     * the high 16 bits the second (total/end), as in i915. */
    gfx_outd(pci, base + 0x00, (m->hactive - 1) | ((htotal - 1) << 16));
    gfx_outd(pci, base + 0x04, (m->hactive - 1) | ((htotal - 1) << 16));
    gfx_outd(pci, base + 0x08,
             (hsync_start - 1) | ((hsync_end - 1) << 16));
    gfx_outd(pci, base + 0x0C, (m->vactive - 1) | ((vtotal - 1) << 16));
    gfx_outd(pci, base + 0x10, (m->vactive - 1) | ((vtotal - 1) << 16));
    gfx_outd(pci, base + 0x14,
             (vsync_start - 1) | ((vsync_end - 1) << 16));
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
    /* The control register self-arms because the plane was just disabled; the
     * surface write that follows arms the whole double-buffered plane state at
     * the next vblank (i915 skl_plane_update_arm writes CTL then SURF). */
    gfx_outd(pci, PLANE_CTL_1_A,
             PLANE_CTL_ENABLE | PLANE_CTL_FORMAT_XRGB8888
             | PLANE_CTL_TILED_LINEAR);
    gfx_outd(pci, PLANE_SURF_1_A, (uint32_t) gfx_addr);
    (void) gfx_ind(pci, PLANE_SURF_1_A);
}

/* Registers the mode set may modify, saved so a failure can restore the
 * firmware's display state instead of leaving a broken signal. */
struct modeset_state {
    uint32_t trans[6];
    uint32_t pipeaconf, pipeasrc, pipemisc, edp_src, edp_pipe_conf,
        edp_pipe_src;
    uint32_t edp_timing[6];
    uint32_t edp_data_m, edp_data_n, edp_link_m, edp_link_n;
    uint32_t trans_ddi_a, trans_ddi_edp, dp_tp_ctl;
    uint32_t ps_ctrl, ps_ctrl2, ps_win_pos, ps_win_pos2, ps_win_sz,
        ps_win_sz2;
    uint32_t plane_ctl, plane_stride, plane_surf, plane_offset, plane_pos,
        plane_size;
    uint32_t blc_ctl, blc_ctl2;
    uint32_t plane_buf_cfg, plane_wm_trans, plane_wm[8];
};

static void modeset_save(gfx_pci_t * pci, struct modeset_state * s)
{
    for (uint32_t i = 0; i < 6; i++)
        s->trans[i] = gfx_ind(pci, 0x60000 + i * 4);
    s->pipeaconf = gfx_ind(pci, PIPEACONF);
    s->pipeasrc = gfx_ind(pci, PIPEASRC);
    s->edp_src = gfx_ind(pci, TRANS_EDP_SRC);
    s->edp_pipe_conf = gfx_ind(pci, TRANS_EDP_PIPE_CONF);
    s->edp_pipe_src = gfx_ind(pci, TRANS_EDP_PIPE_SRC);
    for (uint32_t i = 0; i < 6; i++)
        s->edp_timing[i] = gfx_ind(pci, 0x6F000 + i * 4);
    s->edp_data_m = gfx_ind(pci, TRANS_EDP_DATA_M1);
    s->edp_data_n = gfx_ind(pci, TRANS_EDP_DATA_N1);
    s->edp_link_m = gfx_ind(pci, TRANS_EDP_LINK_M1);
    s->edp_link_n = gfx_ind(pci, TRANS_EDP_LINK_N1);
    s->pipemisc = gfx_ind(pci, PIPE_MISC_A);
    s->trans_ddi_a = gfx_ind(pci, TRANS_DDI_FUNC_CTL_A);
    s->trans_ddi_edp = gfx_ind(pci, TRANS_DDI_FUNC_CTL_EDP);
    s->dp_tp_ctl = gfx_ind(pci, DP_TP_CTL_A);
    s->ps_ctrl = gfx_ind(pci, PS_CTRL_1A);
    s->ps_ctrl2 = gfx_ind(pci, PS_CTRL_2A);
    s->ps_win_pos = gfx_ind(pci, PS_WIN_POS_1A);
    s->ps_win_pos2 = gfx_ind(pci, PS_WIN_POS_2A);
    s->ps_win_sz = gfx_ind(pci, PS_WIN_SZ_1A);
    s->ps_win_sz2 = gfx_ind(pci, PS_WIN_SZ_2A);
    s->plane_ctl = gfx_ind(pci, PLANE_CTL_1_A);
    s->plane_stride = gfx_ind(pci, PLANE_STRIDE_1_A);
    s->plane_surf = gfx_ind(pci, PLANE_SURF_1_A);
    s->plane_offset = gfx_ind(pci, PLANE_OFFSET_1_A);
    s->plane_pos = gfx_ind(pci, PLANE_POS_1_A);
    s->plane_size = gfx_ind(pci, PLANE_SIZE_1_A);
    s->blc_ctl = gfx_ind(pci, BLC_PWM_CTL);
    s->blc_ctl2 = gfx_ind(pci, BLC_PWM_CTL2);
    s->plane_buf_cfg = gfx_ind(pci, PLANE_BUF_CFG_1A);
    s->plane_wm_trans = gfx_ind(pci, PLANE_WM_TRANS_1A);
    for (uint32_t i = 0; i < 8; i++)
        s->plane_wm[i] = gfx_ind(pci, PLANE_WM_1A(i));
}

static void modeset_restore(gfx_pci_t * pci, const struct modeset_state * s)
{
    gfx_outd(pci, PIPEACONF, 0);
    gfx_outd(pci, PLANE_CTL_1_A, 0);
    for (uint32_t i = 0; i < 6; i++)
        gfx_outd(pci, 0x60000 + i * 4, s->trans[i]);
    gfx_outd(pci, PIPEASRC, s->pipeasrc);
    gfx_outd(pci, TRANS_EDP_SRC, s->edp_src);
    gfx_outd(pci, TRANS_EDP_PIPE_SRC, s->edp_pipe_src);
    gfx_outd(pci, TRANS_EDP_PIPE_CONF, s->edp_pipe_conf);
    for (uint32_t i = 0; i < 6; i++)
        gfx_outd(pci, 0x6F000 + i * 4, s->edp_timing[i]);
    gfx_outd(pci, TRANS_EDP_DATA_M1, s->edp_data_m);
    gfx_outd(pci, TRANS_EDP_DATA_N1, s->edp_data_n);
    gfx_outd(pci, TRANS_EDP_LINK_M1, s->edp_link_m);
    gfx_outd(pci, TRANS_EDP_LINK_N1, s->edp_link_n);
    gfx_outd(pci, PIPE_MISC_A, s->pipemisc);
    gfx_outd(pci, TRANS_DDI_FUNC_CTL_A, s->trans_ddi_a);
    gfx_outd(pci, TRANS_DDI_FUNC_CTL_EDP, s->trans_ddi_edp);
    gfx_outd(pci, DP_TP_CTL_A, s->dp_tp_ctl);
    gfx_outd(pci, PS_WIN_POS_1A, s->ps_win_pos);
    gfx_outd(pci, PS_WIN_SZ_1A, s->ps_win_sz);
    gfx_outd(pci, PS_CTRL_1A, s->ps_ctrl);
    gfx_outd(pci, PS_WIN_POS_2A, s->ps_win_pos2);
    gfx_outd(pci, PS_WIN_SZ_2A, s->ps_win_sz2);
    gfx_outd(pci, PS_CTRL_2A, s->ps_ctrl2);
    gfx_outd(pci, PLANE_STRIDE_1_A, s->plane_stride);
    gfx_outd(pci, PLANE_OFFSET_1_A, s->plane_offset);
    gfx_outd(pci, PLANE_POS_1_A, s->plane_pos);
    gfx_outd(pci, PLANE_SIZE_1_A, s->plane_size);
    gfx_outd(pci, PLANE_SURF_1_A, s->plane_surf);
    gfx_outd(pci, PLANE_CTL_1_A, s->plane_ctl);
    gfx_outd(pci, BLC_PWM_CTL, s->blc_ctl);
    gfx_outd(pci, BLC_PWM_CTL2, s->blc_ctl2);
    gfx_outd(pci, PLANE_BUF_CFG_1A, s->plane_buf_cfg);
    gfx_outd(pci, PLANE_WM_TRANS_1A, s->plane_wm_trans);
    for (uint32_t i = 0; i < 8; i++)
        gfx_outd(pci, PLANE_WM_1A(i), s->plane_wm[i]);
    gfx_outd(pci, PIPEACONF, s->pipeaconf);
    klogi("GFX: modeset: restored firmware display state\n");
}

bool skl_edp_set_mode(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                      const display_mode_t * mode, gfx_fb_t * out_fb)
{
    struct modeset_state saved;

    if (mode == NULL || !mode->valid || mode->hactive == 0
        || mode->vactive == 0) {
        kloge("GFX: modeset: invalid mode\n");
        return false;
    }

    modeset_save(pci, &saved);
    klogi("GFX: modeset: %ux%u @ %u Hz (pixel clock %u kHz)\n", mode->hactive,
          mode->vactive, mode->refresh_hz, mode->pixel_clock_khz);

    uint32_t pitch = (mode->hactive * 4 + 63) & ~63u;
    uint64_t fbsize = (uint64_t) pitch * mode->vactive;

    gfx_object_t obj = { 0 };
    if (!gfx_alloc(mgr, gtt, &obj, fbsize, 64)) {
        kloge("GFX: modeset: framebuffer allocation (%lu bytes) failed\n",
              fbsize);
        return false;
    }
    /* Clear the framebuffer; the console redraws the visible content once it
     * takes over the scanout. */
    memset((void *) obj.cpu_addr, 0, fbsize);
    /* The framebuffer is mapped through the cacheable direct map while the
     * display engine reads it from DRAM, so push the writes out first. */
    asm volatile ("wbinvd":::"memory");

    klogi("GFX: modeset: fb 0x%016lx (%ux%u pitch %u)\n",
          obj.gfx_addr, mode->hactive, mode->vactive, pitch);

    /* Keep the firmware's display core up; only turn it on if it is off. */
    if (!power_well_on(pci, PWR_WELL_CTL1, PWR_WELL_CTL1_DC_REQ,
                       PWR_WELL_CTL1_DC_STATE, "DC"))
        goto fail;
    if (!power_well_on(pci, PWR_WELL_CTL2, PWR_WELL_DDI_A_REQ,
                       PWR_WELL_DDI_A_STATE, "DDI-A"))
        goto fail;

    /* The GOP trained the eDP link and enabled DPLL0; require that. */
    if (!(gfx_ind(pci, DPLL_STATUS) & DPLL0_LOCK)) {
        kloge("GFX: modeset: DPLL0 not locked (status 0x%08x)\n",
              gfx_ind(pci, DPLL_STATUS));
        goto fail;
    }

    if (!panel_power_on(pci))
        goto fail;

    /* First stop the plane, pipe, eDP transcoder and DP transport, so the
     * programming below starts from a clean, fully disabled state. This
     * reverse teardown (plane -> pipe -> transcoder -> DDI -> PLL) runs once
     * when the driver takes the pipe over from the firmware. */
    gfx_outd(pci, PLANE_CTL_1_A, 0);
    gfx_outd(pci, 0x70080, 0);  /* hardware cursor off */
    gfx_outd(pci, PIPEACONF, 0);
    /* The eDP transcoder's shifted pipe config drives the output, so it is
     * disabled (and polled off) together with the pipe. */
    gfx_outd(pci, TRANS_EDP_PIPE_CONF, 0);
    /* command.txt: the pipe must be confirmed off before the clock tree
     * (CDCLK/PLL) is touched, so treat a failure here as fatal. */
    pit_wait(20);
    if (!wait_pipe_off(pci))
        goto fail;
    gfx_outd(pci, TRANS_DDI_FUNC_CTL_EDP, 0);
    gfx_outd(pci, DDI_BUF_CTL_A, 0);
    gfx_outd(pci, DP_TP_CTL_A, DP_TP_CTL_ENABLE | DP_TP_CTL_LINK_TRAIN_IDLE);
    pit_wait(1);
    gfx_outd(pci, DP_TP_CTL_A, 0);
    pit_wait(1);
    /* PLL / clock: gate the port's DDI clock (i915 skl_ddi_disable_clock).
     * LCPLL1 itself stays on because it also feeds CDCLK. */
    gfx_outd(pci, DPLL_CTRL2,
             gfx_ind(pci, DPLL_CTRL2) | DPLL_CTRL2_DDI_CLK_OFF(0));
    pit_wait(1);

    /* Bring the display out of the DC5/DC6 low-power states so the pipe
     * registers are not gated. */
    gfx_outd(pci, DC_STATE_EN, DC_STATE_DISABLE);

    /* Power the sink's main link to D0 (i915's intel_dp_set_power). */
    {
        static const uint8_t d0[1] = { 0x01 };

        if (!aux_dpcd_write(pci, 0x600, d0, 1))
            klogw("GFX: modeset: DPCD 0x600 (D0) write failed\n");
    }

    /* Configure order per command.txt: CDCLK -> PLL -> DDI_BUF_TRANS -> AUX
     * training. CDCLK keeps the firmware's value. */
    gfx_outd(pci, CDCLK_CTL, gfx_ind(pci, CDCLK_CTL));

    /* PLL (DPLL0 / LCPLL1): it stays on (it also feeds CDCLK); the port's DDI
     * clock is re-enabled in the enable phase via DPLL_CTRL2. */

    /* Select DPLL0 as the port A DDI clock and ungate it (i915
     * skl_ddi_enable_clock). */
    gfx_outd(pci, DPLL_CTRL2,
             (gfx_ind(pci, DPLL_CTRL2)
              & ~(DPLL_CTRL2_DDI_CLK_OFF(0)
                  | DPLL_CTRL2_DDI_CLK_SEL_MASK(0)))
             | DPLL_CTRL2_DDI_CLK_SEL(0, 0)
             | DPLL_CTRL2_DDI_SEL_OVERRIDE(0));

    /* DDI_BUF_TRANS: keep the firmware's 7-entry buffer translation table. */
    for (uint32_t i = 0; i < 7; i++)
        gfx_outd(pci, 0x64E00 + i * 4, gfx_ind(pci, 0x64E00 + i * 4));

    /* The firmware already trained the eDP link, and a resolution change does
     * not need re-training: the link rate / symbol clock is unchanged (see
     * command.txt). Keep the trained link untouched. */

    /* The source size holds width in the high half and height in the low half.
     * Program it for the pipe, the eDP transcoder's own timing block and its
     * shifted pipe register, so nothing keeps scanning the firmware's smaller
     * boot region. i915 writes the pipe source (intel_set_pipe_src_size) before
     * the transcoder timings (hsw_configure_cpu_transcoder), so do the same. */
    gfx_outd(pci, PIPEASRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    gfx_outd(pci, TRANS_EDP_SRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    gfx_outd(pci, TRANS_EDP_PIPE_SRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    transcoder_timing(pci, 0, mode);
    /* The active eDP transcoder (base 0x6F000) keeps its own timing copy. */
    transcoder_timing(pci, 15, mode);

    /* The transcoder timing and link clock are unchanged, so the firmware's
     * M/N is still valid and is written back verbatim: it paces the link for
     * the panel's real colour depth (the firmware data M/N matches 18bpp, i.e.
     * 6bpc). Recomputing it for 24bpp desynchronises the sink and the image
     * ripples. LINK_N1 arms the double-buffered M/N update, so write it last. */
    gfx_outd(pci, TRANS_EDP_DATA_M1, saved.edp_data_m);
    gfx_outd(pci, TRANS_EDP_DATA_N1, saved.edp_data_n);
    gfx_outd(pci, TRANS_EDP_LINK_M1, saved.edp_link_m);
    gfx_outd(pci, TRANS_EDP_LINK_N1, saved.edp_link_n);
    /* Likewise keep the firmware's PIPE_MISC, which encodes the panel colour
     * depth and dithering (0x50 = 6bpc + dither) and must agree with the
     * link pacing. */
    gfx_outd(pci, PIPE_MISC_A, saved.pipemisc);

    /* Keep the sink's negotiated enhanced framing, which the firmware had
     * enabled; dropping it desynchronises the sink. */
    gfx_outd(pci, DP_TP_CTL_A,
             DP_TP_CTL_ENABLE | DP_TP_CTL_MODE_SST
             | (saved.dp_tp_ctl & DP_TP_CTL_ENHANCED_FRAME_ENABLE)
             | DP_TP_CTL_LINK_TRAIN_NORMAL);
    pit_wait(1);
    /* Enable order per command.txt: DDI_BUF_CTL -> transcoder -> pipe -> plane. */
    gfx_outd(pci, DDI_BUF_CTL_A,
             DDI_BUF_CTL_ENABLE | DDI_BUF_CTL_PORT_WIDTH_X1);

    gfx_outd(pci, TRANS_DDI_FUNC_CTL_EDP, saved.trans_ddi_edp);
    /* MSA MISC for the eDP transcoder: 8bpc, sync clock (i915
     * intel_ddi_set_dp_msa). */
    gfx_outd(pci, 0x6F410, 0x01);

    /* The eDP transcoder's shifted pipe config (0x7F008) is what actually
     * drives the DDI output, so enable it as well (i915 intel_enable_transcoder
     * for TRANSCODER_EDP). Written disabled first so the state machine
     * transitions cleanly during a full mode set. */
    gfx_outd(pci, TRANS_EDP_PIPE_CONF, PIPE_PROGRESSIVE);
    gfx_outd(pci, TRANS_EDP_PIPE_CONF,
             gfx_ind(pci, TRANS_EDP_PIPE_CONF) | PIPE_ENABLE
             | PIPE_PROGRESSIVE);

    /* hsw_set_transconf writes PIPECONF disabled first during a modeset, so the
     * state machine transitions cleanly when we enable it. */
    gfx_outd(pci, PIPEACONF, PIPE_PROGRESSIVE);
    gfx_outd(pci, PIPEACONF,
             gfx_ind(pci, PIPEACONF) | PIPE_ENABLE | PIPE_PROGRESSIVE);
    for (int i = 0; i < 100 && !(gfx_ind(pci, PIPEACONF) & PIPE_STATE); i++)
        pit_wait(1);
    if (!pipe_is_running(pci)) {
        kloge("GFX: modeset: pipe A did not start\n");
        goto fail;
    }

    /* The pipe source equals the active timing (1:1), so no panel fitting is
     * needed. Detach both pipe scalers like i915's skl_scaler_disable(): the
     * firmware left one scaling its 800x600 boot mode up to the panel, and its
     * scaling phase would otherwise scale the native source a second time. */
    gfx_outd(pci, PS_CTRL_1A, 0);
    gfx_outd(pci, PS_WIN_POS_1A, 0);
    gfx_outd(pci, PS_WIN_SZ_1A, 0);
    gfx_outd(pci, PS_CTRL_2A, 0);
    gfx_outd(pci, PS_WIN_POS_2A, 0);
    gfx_outd(pci, PS_WIN_SZ_2A, 0);
    /* The firmware sized the plane's data buffer and watermark for its small
     * boot plane, so give the plane the whole display data buffer and a
     * generous FIFO watermark to sustain the full-resolution fetch. */
    gfx_outd(pci, PLANE_BUF_CFG_1A, PLANE_BUF_END(445) | PLANE_BUF_START(0));
    for (uint32_t lvl = 0; lvl < 8; lvl++)
        gfx_outd(pci, PLANE_WM_1A(lvl),
                 PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(32));
    gfx_outd(pci, PLANE_WM_TRANS_1A,
             PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(32));

    plane_configure(pci, mode, obj.gfx_addr, pitch);
    wait_vblank(pci);
    /* Re-arm the source size on the pipe, both eDP transcoder copies and the
     * plane window once the pipe and plane are running: enabling the eDP pipe
     * resets the source to the firmware's small boot region, so a write done
     * before enable does not stick and must be repeated at a vblank. */
    gfx_outd(pci, PIPEASRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    gfx_outd(pci, TRANS_EDP_SRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    gfx_outd(pci, TRANS_EDP_PIPE_SRC,
             (mode->hactive - 1) << 16 | (mode->vactive - 1));
    gfx_outd(pci, PLANE_SIZE_1_A,
             (mode->vactive - 1) << 16 | (mode->hactive - 1));
    gfx_outd(pci, PLANE_SURF_1_A, (uint32_t) obj.gfx_addr);
    wait_vblank(pci);
    backlight_on(pci, 0xFFFF);

    /* refresh = pixel_clock / (htotal * vtotal); measuring it reveals the
     * pixel clock the pipe is actually running at (and thus whether the DP
     * M/N ratio is being honoured). */
    {
        uint32_t htotal = mode->hactive + mode->hblank;
        uint32_t vtotal = mode->vactive + mode->vblank;
        uint32_t f0 = gfx_ind(pci, 0x70040);

        pit_wait(200);
        uint32_t fps = (gfx_ind(pci, 0x70040) - f0) * 5;
        klogi("GFX: modeset: measured %u fps (pixel clock ~%u kHz)\n", fps,
              (uint32_t) ((uint64_t) fps * htotal * vtotal / 1000));
    }

    out_fb->obj = obj;
    out_fb->width = mode->hactive;
    out_fb->height = mode->vactive;
    out_fb->stride = pitch;
    out_fb->format = DISPPLANE_BGRX888;

    klogi("GFX: modeset: %ux%u @ %u Hz done\n", mode->hactive, mode->vactive,
          mode->refresh_hz);
    skl_display_dump(pci);
    return true;

  fail:
    modeset_restore(pci, &saved);
    return false;
}

/* Minimal DP AUX native read of the DPCD (diagnostics only). */
static bool aux_dpcd_read(gfx_pci_t * pci, uint32_t addr, uint8_t * buf,
                          uint32_t len)
{
    uint8_t tx[4], rx[20];
    uint32_t i, j;

    if (len == 0 || len > 16)
        return false;

    tx[0] = (0x9 << 4) | ((addr >> 16) & 0xf); /* DP_AUX_NATIVE_READ */
    tx[1] = (addr >> 8) & 0xff;
    tx[2] = addr & 0xff;
    tx[3] = (uint8_t) (len - 1);
    gfx_outd(pci, DDI_AUX_DATA_A0,
             ((uint32_t) tx[0] << 24) | ((uint32_t) tx[1] << 16)
             | ((uint32_t) tx[2] << 8) | (uint32_t) tx[3]);

    gfx_outd(pci, DDI_AUX_CTL_A,
             (gfx_ind(pci, DDI_AUX_CTL_A) & ~DDI_AUX_CTL_MSG_SIZE(0x1f))
             | DDI_AUX_CTL_SEND_BUSY | DDI_AUX_CTL_DONE
             | DDI_AUX_CTL_INTERRUPT | DDI_AUX_CTL_TIME_OUT_MAX
             | DDI_AUX_CTL_RECEIVE_ERROR | DDI_AUX_CTL_MSG_SIZE(4));

    for (i = 0; i < 1000000; i++) {
        uint32_t st = gfx_ind(pci, DDI_AUX_CTL_A);

        if (st & (DDI_AUX_CTL_DONE | DDI_AUX_CTL_TIME_OUT_ERROR
                  | DDI_AUX_CTL_RECEIVE_ERROR))
            break;
    }

    if ((gfx_ind(pci, DDI_AUX_CTL_A) & DDI_AUX_CTL_DONE) == 0)
        return false;

    /* The reply is one status byte followed by the requested data; the AUX
     * data registers are big-endian. */
    for (i = 0; i < len + 1; i += 4) {
        uint32_t v = gfx_ind(pci, DDI_AUX_DATA_A0 + (i / 4) * 4);

        for (j = 0; j < 4 && i + j < len + 1; j++)
            rx[i + j] = (uint8_t) (v >> (8 * (3 - j)));
    }
    for (i = 0; i < len; i++)
        buf[i] = rx[i + 1];

    return true;
}

/* Minimal DP AUX native write of the DPCD. */
static bool aux_dpcd_write(gfx_pci_t * pci, uint32_t addr, const uint8_t * buf,
                           uint32_t len)
{
    uint8_t tx[20];
    uint32_t i, j;

    if (len == 0 || len > 16)
        return false;

    tx[0] = (0x8 << 4) | ((addr >> 16) & 0xf); /* DP_AUX_NATIVE_WRITE */
    tx[1] = (addr >> 8) & 0xff;
    tx[2] = addr & 0xff;
    tx[3] = (uint8_t) (len - 1);
    for (i = 0; i < len; i++)
        tx[4 + i] = buf[i];

    for (i = 0; i < len + 4; i += 4) {
        uint32_t v = 0;

        for (j = 0; j < 4 && i + j < len + 4; j++)
            v |= (uint32_t) tx[i + j] << (8 * (3 - j));
        gfx_outd(pci, DDI_AUX_DATA_A0 + (i / 4) * 4, v);
    }

    gfx_outd(pci, DDI_AUX_CTL_A,
             (gfx_ind(pci, DDI_AUX_CTL_A) & ~DDI_AUX_CTL_MSG_SIZE(0x1f))
             | DDI_AUX_CTL_SEND_BUSY | DDI_AUX_CTL_DONE
             | DDI_AUX_CTL_INTERRUPT | DDI_AUX_CTL_TIME_OUT_MAX
             | DDI_AUX_CTL_RECEIVE_ERROR | DDI_AUX_CTL_MSG_SIZE(len + 4));

    for (i = 0; i < 1000000; i++) {
        uint32_t st = gfx_ind(pci, DDI_AUX_CTL_A);

        if (st & (DDI_AUX_CTL_DONE | DDI_AUX_CTL_TIME_OUT_ERROR
                  | DDI_AUX_CTL_RECEIVE_ERROR))
            break;
    }

    return (gfx_ind(pci, DDI_AUX_CTL_A) & DDI_AUX_CTL_DONE) != 0;
}

/* A concise summary of the display state that matters for a mode set: the
 * clocks, the pipe/plane geometry, the eDP link/M/N and the backlight. */
void skl_display_dump(gfx_pci_t * pci)
{
    uint8_t d[8];

    if (aux_dpcd_read(pci, 0x000, d, 8))
        klogi("GFX: display: DPCD 000 %02x %02x %02x %02x %02x %02x %02x "
              "%02x\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    if (aux_dpcd_read(pci, 0x100, d, 8))
        klogi("GFX: display: DPCD 100 %02x %02x %02x %02x %02x %02x %02x "
              "%02x\n", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    klogi("GFX: display: PWR_WELL1 0x%08x PWR_WELL2 0x%08x CDCLK 0x%08x "
          "LCPLL1 0x%08x DPLL_STATUS 0x%08x\n", gfx_ind(pci, PWR_WELL_CTL1),
          gfx_ind(pci, PWR_WELL_CTL2), gfx_ind(pci, CDCLK_CTL),
          gfx_ind(pci, LCPLL1_CTL), gfx_ind(pci, DPLL_STATUS));
    klogi("GFX: display: PIPEACONF 0x%08x PIPEASRC 0x%08x PIPESTAT 0x%08x "
          "TRANS_DDI 0x%08x MSA_MISC 0x%08x\n", gfx_ind(pci, PIPEACONF),
          gfx_ind(pci, PIPEASRC), gfx_ind(pci, 0x70024),
          gfx_ind(pci, TRANS_DDI_FUNC_CTL_EDP), gfx_ind(pci, 0x6F410));
    klogi("GFX: display: eDP HTOTAL 0x%08x VTOTAL 0x%08x SRC 0x%08x "
          "pipe_conf 0x%08x\n", gfx_ind(pci, 0x6F000), gfx_ind(pci, 0x6F00C),
          gfx_ind(pci, TRANS_EDP_SRC), gfx_ind(pci, TRANS_EDP_PIPE_CONF));
    klogi("GFX: display: eDP M/N data 0x%08x/0x%08x link 0x%08x/0x%08x\n",
          gfx_ind(pci, TRANS_EDP_DATA_M1), gfx_ind(pci, TRANS_EDP_DATA_N1),
          gfx_ind(pci, TRANS_EDP_LINK_M1), gfx_ind(pci, TRANS_EDP_LINK_N1));
    klogi("GFX: display: PLANE_CTL 0x%08x SIZE 0x%08x STRIDE 0x%08x POS "
          "0x%08x SURF 0x%08x\n", gfx_ind(pci, PLANE_CTL_1_A),
          gfx_ind(pci, PLANE_SIZE_1_A), gfx_ind(pci, PLANE_STRIDE_1_A),
          gfx_ind(pci, PLANE_POS_1_A), gfx_ind(pci, PLANE_SURF_1_A));
    klogi("GFX: display: PIPE_MISC 0x%08x PS_CTRL 0x%08x PS_WIN_SZ 0x%08x "
          "DDI_BUF_CTL 0x%08x DP_TP_CTL 0x%08x\n", gfx_ind(pci, PIPE_MISC_A),
          gfx_ind(pci, PS_CTRL_1A), gfx_ind(pci, PS_WIN_SZ_1A),
          gfx_ind(pci, DDI_BUF_CTL_A), gfx_ind(pci, 0x64040));
    klogi("GFX: display: PP_STATUS 0x%08x BLC_PWM_CTL2 0x%08x\n",
          gfx_ind(pci, PP_STATUS), gfx_ind(pci, BLC_PWM_CTL2));
    {
        uint32_t a = gfx_ind(pci, 0x70040);

        pit_wait(40);
        klogi("GFX: display: pipe A frame delta %u (40 ms)\n",
              gfx_ind(pci, 0x70040) - a);
    }
}
