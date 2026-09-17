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

/* Select the transcoder's input clock (normally DPLL0 for eDP). Only
 * Haswell/Broadwell expose TRANS_CLK_SEL; Skylake selects the clock through
 * the DDI/DPLL mapping, so this is kept for reference. */
static bool __attribute__((unused)) transcoder_clock_select(gfx_pci_t * pci,
                                                            uint8_t tr,
                                                            uint32_t sel,
                                                            const char *name)
{
    uint32_t reg = TRANS_CLK_SEL_A + (uint32_t) tr * 4;
    uint32_t v = gfx_ind(pci, reg);

    if ((v & TRANS_CLK_SEL_MASK) == sel) {
        klogd("GFX: modeset: %s already selected\n", name);
        return true;
    }

    gfx_outd(pci, reg, (v & ~TRANS_CLK_SEL_MASK) | sel);
    return wait_bits(pci, reg, TRANS_CLK_SEL_MASK, sel, name);
}

/* True while the pipe's frame counter keeps advancing (the pipe is scanning). */
static bool pipe_is_running(gfx_pci_t * pci)
{
    uint32_t a = gfx_ind(pci, 0x70040);
    pit_wait(40);
    return gfx_ind(pci, 0x70040) != a;
}

/* Wait for the next vblank (frame counter advance), bounded. */
static void __attribute__((unused)) wait_vblank(gfx_pci_t * pci)
{
    uint32_t a = gfx_ind(pci, 0x70040);
    for (int i = 0; i < 40 && gfx_ind(pci, 0x70040) == a; i++)
        pit_wait(1);
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
    if (!wait_bits(pci, PIPEACONF, PIPE_STATE, PIPE_STATE, "pipe A enable")) {
        gfx_outd(pci, PIPEACONF, 0);
        return false;
    }
    return true;
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

/* DP M/N values: the ratio the pipe uses to regenerate its pixel clock from
 * the link symbol clock, and the data-clock ratio. Mirrors i915's
 * intel_link_compute_m_n(). */
struct link_m_n {
    uint32_t tu;
    uint32_t data_m, data_n;
    uint32_t link_m, link_n;
};

static void compute_m_n(uint32_t * ret_m, uint32_t * ret_n, uint32_t m,
                        uint32_t n, uint32_t constant_n)
{
    *ret_n = constant_n;
    *ret_m = (uint32_t) (((uint64_t) m * (*ret_n)) / n);

    while (*ret_m > DATA_LINK_M_N_MASK || *ret_n > DATA_LINK_M_N_MASK) {
        *ret_m >>= 1;
        *ret_n >>= 1;
    }
}

static void link_compute_m_n(uint16_t bpp, int nlanes, int pixel_clock,
                             int link_clock, struct link_m_n * mn)
{
    uint32_t data_clock = (uint32_t) bpp * (uint32_t) pixel_clock;

    mn->tu = 64;
    compute_m_n(&mn->data_m, &mn->data_n, data_clock,
                (uint32_t) link_clock * (uint32_t) nlanes * 8, 0x8000000);
    compute_m_n(&mn->link_m, &mn->link_n, (uint32_t) pixel_clock,
                (uint32_t) link_clock, 0x80000);
}

static void program_m_n(gfx_pci_t * pci, const struct link_m_n * mn)
{
    gfx_outd(pci, PIPE_DATA_M1_A, TU_SIZE(mn->tu) | mn->data_m);
    gfx_outd(pci, PIPE_DATA_N1_A, mn->data_n);
    gfx_outd(pci, PIPE_LINK_M1_A, mn->link_m);
    /* LINK_N1 arms the double-buffered M/N update, so it is written last. */
    gfx_outd(pci, PIPE_LINK_N1_A, mn->link_n);
}

/* Registers the mode set may modify, saved so a failure can restore the
 * firmware's display state instead of leaving a broken signal. */
struct modeset_state {
    uint32_t trans[6];
    uint32_t pipeaconf, pipeasrc, pipemisc;
    uint32_t trans_ddi_a, trans_dp_a;
    uint32_t plane_ctl, plane_stride, plane_surf, plane_offset, plane_pos,
        plane_size;
    uint32_t blc_ctl, blc_ctl2;
};

static void modeset_save(gfx_pci_t * pci, struct modeset_state * s)
{
    for (uint32_t i = 0; i < 6; i++)
        s->trans[i] = gfx_ind(pci, 0x60000 + i * 4);
    s->pipeaconf = gfx_ind(pci, PIPEACONF);
    s->pipeasrc = gfx_ind(pci, PIPEASRC);
    s->pipemisc = gfx_ind(pci, PIPE_MISC_A);
    s->trans_ddi_a = gfx_ind(pci, TRANS_DDI_FUNC_CTL_A);
    s->trans_dp_a = gfx_ind(pci, TRANS_DP_CTL_A);
    s->plane_ctl = gfx_ind(pci, PLANE_CTL_1_A);
    s->plane_stride = gfx_ind(pci, PLANE_STRIDE_1_A);
    s->plane_surf = gfx_ind(pci, PLANE_SURF_1_A);
    s->plane_offset = gfx_ind(pci, PLANE_OFFSET_1_A);
    s->plane_pos = gfx_ind(pci, PLANE_POS_1_A);
    s->plane_size = gfx_ind(pci, PLANE_SIZE_1_A);
    s->blc_ctl = gfx_ind(pci, BLC_PWM_CTL);
    s->blc_ctl2 = gfx_ind(pci, BLC_PWM_CTL2);
}

static void modeset_restore(gfx_pci_t * pci, const struct modeset_state * s)
{
    gfx_outd(pci, PIPEACONF, 0);
    gfx_outd(pci, PLANE_CTL_1_A, 0);
    for (uint32_t i = 0; i < 6; i++)
        gfx_outd(pci, 0x60000 + i * 4, s->trans[i]);
    gfx_outd(pci, PIPEASRC, s->pipeasrc);
    gfx_outd(pci, PIPE_MISC_A, s->pipemisc);
    gfx_outd(pci, TRANS_DDI_FUNC_CTL_A, s->trans_ddi_a);
    gfx_outd(pci, TRANS_DP_CTL_A, s->trans_dp_a);
    gfx_outd(pci, PLANE_STRIDE_1_A, s->plane_stride);
    gfx_outd(pci, PLANE_OFFSET_1_A, s->plane_offset);
    gfx_outd(pci, PLANE_POS_1_A, s->plane_pos);
    gfx_outd(pci, PLANE_SIZE_1_A, s->plane_size);
    gfx_outd(pci, PLANE_SURF_1_A, s->plane_surf);
    gfx_outd(pci, PLANE_CTL_1_A, s->plane_ctl);
    gfx_outd(pci, BLC_PWM_CTL, s->blc_ctl);
    gfx_outd(pci, BLC_PWM_CTL2, s->blc_ctl2);
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
    klogi("GFX: modeset: firmware HTOTAL 0x%08x VTOTAL 0x%08x PIPEACONF 0x%08x\n",
          saved.trans[0], saved.trans[3], saved.pipeaconf);

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

    bool running = pipe_is_running(pci);
    if (running) {
        /* The firmware keeps pipe A scanning. The pipe regenerates its pixel
         * clock from the link symbol clock through the DP M/N, so reprogram
         * M/N, timing, source size and plane in place (latched at vblank). */
        uint32_t ddi = gfx_ind(pci, DDI_BUF_CTL_A);
        int nlanes = (int) ((ddi & DDI_BUF_CTL_PORT_WIDTH_MASK) >> 1) + 1;
        struct link_m_n mn;

        link_compute_m_n(24, nlanes, (int) mode->pixel_clock_khz, 270000,
                         &mn);
        klogi("GFX: modeset: in-place M/N data %u/%u link %u/%u (lanes %d, "
              "pixel clock %u kHz)\n", mn.data_m, mn.data_n, mn.link_m,
              mn.link_n, nlanes, mode->pixel_clock_khz);

        program_m_n(pci, &mn);
        transcoder_timing(pci, 0, mode);
        gfx_outd(pci, PIPEASRC,
                 (mode->vactive - 1) << 16 | (mode->hactive - 1));
        wait_vblank(pci);
        plane_configure(pci, mode, obj.gfx_addr, pitch);
        wait_vblank(pci);
        backlight_on(pci, 0xFFFF);

        klogi("GFX: modeset: M/N readback D-M1 0x%08x D-N1 0x%08x L-M1 0x%08x "
              "L-N1 0x%08x\n", gfx_ind(pci, PIPE_DATA_M1_A),
              gfx_ind(pci, PIPE_DATA_N1_A), gfx_ind(pci, PIPE_LINK_M1_A),
              gfx_ind(pci, PIPE_LINK_N1_A));
        klogi("GFX: modeset: readback HTOTAL 0x%08x VTOTAL 0x%08x PIPEASRC "
              "0x%08x PLANE_CTL 0x%08x\n", gfx_ind(pci, PIPE_HTOTAL(0)),
              gfx_ind(pci, PIPE_VTOTAL(0)), gfx_ind(pci, PIPEASRC),
              gfx_ind(pci, PLANE_CTL_1_A));
    } else {
        transcoder_timing(pci, 0, mode);
        transcoder_ddi_enable(pci, mode);
        if (!pipe_configure(pci, mode))
            goto fail;
        plane_configure(pci, mode, obj.gfx_addr, pitch);
        backlight_on(pci, 0xFFFF);
    }

    out_fb->obj = obj;
    out_fb->width = mode->hactive;
    out_fb->height = mode->vactive;
    out_fb->stride = pitch;
    out_fb->format = DISPPLANE_BGRX888;

    klogi("GFX: modeset: %ux%u @ %u Hz done\n", mode->hactive, mode->vactive,
          mode->refresh_hz);
    return true;

  fail:
    modeset_restore(pci, &saved);
    return false;
}

/* Log the non-zero words in a display register range (read-only scan). */
static void dump_region(gfx_pci_t * pci, uint32_t base, uint32_t bytes)
{
    for (uint32_t off = 0; off < bytes; off += 4) {
        uint32_t v = gfx_ind(pci, base + off);
        if (v != 0)
            klogi("    [0x%05x] 0x%08x\n", base + off, v);
    }
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
    klogi("  PIPEBCONF 0x%08x PIPECCONF 0x%08x\n", gfx_ind(pci, 0x71008),
          gfx_ind(pci, 0x72008));
    klogi("  TRANS_CLK_SEL A/B/C 0x%08x 0x%08x 0x%08x\n",
          gfx_ind(pci, TRANS_CLK_SEL_A), gfx_ind(pci, TRANS_CLK_SEL_B),
          gfx_ind(pci, TRANS_CLK_SEL_C));
    klogi("  DPLL_CTRL1 0x%08x DPLL_CTRL2 0x%08x\n",
          gfx_ind(pci, DPLL_CTRL1), gfx_ind(pci, DPLL_CTRL2));
    klogi("  TRANS_DDI 0x%08x PLANE_CTL 0x%08x PLANE_SURF 0x%08x\n",
          gfx_ind(pci, TRANS_DDI_FUNC_CTL_A), gfx_ind(pci, PLANE_CTL_1_A),
          gfx_ind(pci, PLANE_SURF_1_A));
    klogi("  PP_STATUS 0x%08x PP_CONTROL 0x%08x\n", gfx_ind(pci, PP_STATUS),
          gfx_ind(pci, PP_CONTROL));
    klogi("  DDI_BUF_CTL_A 0x%08x DDI_AUX_CTL_A 0x%08x\n",
          gfx_ind(pci, DDI_BUF_CTL_A), gfx_ind(pci, DDI_AUX_CTL_A));
    klogi("  DDI_BUF A/B/C/D 0x%08x 0x%08x 0x%08x 0x%08x\n",
          gfx_ind(pci, 0x64000), gfx_ind(pci, 0x64100), gfx_ind(pci, 0x64200),
          gfx_ind(pci, 0x64300));
    klogi("  TRANS_DDI A/B/C 0x%08x 0x%08x 0x%08x\n",
          gfx_ind(pci, 0x60400), gfx_ind(pci, 0x61400), gfx_ind(pci, 0x62400));
    klogi("  PLANE_CTL A/B/C 0x%08x 0x%08x 0x%08x\n",
          gfx_ind(pci, 0x70180), gfx_ind(pci, 0x71180), gfx_ind(pci, 0x72180));
    klogi("  PLANE_SURF A/B/C 0x%08x 0x%08x 0x%08x\n",
          gfx_ind(pci, 0x7019C), gfx_ind(pci, 0x7119C), gfx_ind(pci, 0x7219C));
    klogi("  VGA_CONTROL 0x%08x BLC_PWM_CTL2 0x%08x BLC_PWM_CTL 0x%08x\n",
          gfx_ind(pci, VGA_CONTROL), gfx_ind(pci, BLC_PWM_CTL2),
          gfx_ind(pci, BLC_PWM_CTL));
    klogi("  PIPESRC B/C 0x%08x 0x%08x\n", gfx_ind(pci, 0x6101C),
          gfx_ind(pci, 0x6201C));
    klogi("  TIMING A/B/C HTOTAL 0x%08x 0x%08x 0x%08x VTOTAL 0x%08x 0x%08x "
          "0x%08x\n", gfx_ind(pci, 0x60000), gfx_ind(pci, 0x61000),
          gfx_ind(pci, 0x62000), gfx_ind(pci, 0x6000C), gfx_ind(pci, 0x6100C),
          gfx_ind(pci, 0x6200C));
    {
        uint32_t a = gfx_ind(pci, 0x70040);
        uint32_t b = gfx_ind(pci, 0x71040);
        uint32_t c = gfx_ind(pci, 0x72040);
        pit_wait(40);
        klogi("  FRMCOUNT delta A/B/C %u %u %u (40 ms)\n",
              gfx_ind(pci, 0x70040) - a, gfx_ind(pci, 0x71040) - b,
              gfx_ind(pci, 0x72040) - c);
    }
    klogi("  clock region 0x46000:\n");
    dump_region(pci, 0x46000, 0x200);
    klogi("  DPLL region 0x6C000:\n");
    dump_region(pci, 0x6C000, 0x80);
    klogi("  trans A region 0x60000:\n");
    dump_region(pci, 0x60000, 0x100);
    klogi("  pipe/plane A region 0x70000:\n");
    dump_region(pci, 0x70000, 0x100);
}
