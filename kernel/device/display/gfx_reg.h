/**-----------------------------------------------------------------------------

 @file    gfx_reg.h
 @brief   Graphics device register definitions
 @details
 @verbatim

  This file contains the definitions of various registers and constants used
  for handling graphics devices within the HanOS kernel. It includes bit masks
  and shifts for manipulating register values, as well as addresses for memory
  mapped I/O registers related to graphics memory management, GTT (Graphics
  Translation Table), and display control.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define MASKED_ENABLE(x)                (((x) << 16) | (x))
#define MASKED_DISABLE(x)               ((x) << 16)

/* 2.1.2.1 GTT Page Table Entries */

#define GTT_PAGE_SHIFT                  12
#define GTT_PAGE_SIZE                   (1 << GTT_PAGE_SHIFT)

#define GTT_ENTRY_VALID                 (1 << 0)
#define GTT_ENTRY_L3_CACHE_CONTROL      (1 << 1)
#define GTT_ENTRY_LLC_CACHE_CONTROL     (1 << 2)
#define GTT_ENTRY_GFX_DATA_TYPE         (1 << 3)
#define GTT_ENTRY_ADDR(x)               ((x) | ((x >> 28) & 0xff0))

/* 3. GFX MMIO - MCHBAR Aperture */
#define GFX_MCHBAR                      0x140000

/* Vol 1. Part 3. Memory Interface and Commands for the Render Engine */
/* 1.1.1.1 ARB_MODE – Arbiter Mode Control register */

#define ARB_MODE                        0x04030 /* R/W */

#define ARB_MODE_GGTAGDR                (1 << 0)        /* GTT Accesses GDR */
#define ARB_MODE_CCGDREN                (1 << 1)        /* Color Cache GDR Enable Bit */
#define ARB_MODE_DCGDREN                (1 << 2)        /* Depth Cache GDR Enable Bit */
#define ARB_MODE_TCGDREN                (1 << 3)        /* Texture Cache GDR Enable Bit */
#define ARB_MODE_VMC_GDR_EN             (1 << 4)        /* VMC GDR Enable */
#define ARB_MODE_AS4TS                  (1 << 5)        /* Address Swizzling for Tiled Surfaces */
#define ARB_MODE_CDPS                   (1 << 8)        /* Color/Depth Port Share Bit */
#define ARB_MODE_GAMPD_GDR              (1 << 9)        /* GAM PD GDR */
#define ARB_MODE_BLB_GDR                (1 << 10)       /* BLB GDR */
#define ARB_MODE_STC_GDR                (1 << 11)       /* STC GDR */
#define ARB_MODE_HIZ_GDR                (1 << 12)       /* HIZ GDR */
#define ARB_MODE_DC_GDR                 (1 << 13)       /* DC GDR */
#define ARB_MODE_GAM2BGTTT              (1 << 14)       /* GAM to Bypass GTT Translation */

/* Vol 3. Part 1. VGA and Extended VGA Registers */
/* 1.2.1 Sequencer Index */

#define SR_INDEX                        0x3c4
#define SR_DATA                         0x3c5

/* 1.2.3 Clocking Mode */

#define SEQ_CLOCKING                    0x01
#define SCREEN_OFF                      (1 << 5)

/* Vol 3. Part 2. PCI Registers */
/* 1.25 MGGC0 - Mirror of GMCH Graphics Control Register */

#define MGGC0                           0x50    /* In PCI Config Space */

#define GGC_LOCK                        (1 << 0)
#define GGC_IVD                         (1 << 1)        /* IGD VGA Disable */
#define GGC_GMS_SHIFT                   3       /* Graphics Mode Select */
#define GGC_GMS_MASK                    0x1f
#define GGC_GGMS_SHIFT                  8       /* GTT Graphics Memory Size */
#define GGC_GGMS_MASK                   0x3
#define GGC_VAMEN                       (1 << 14)       /* Versatile Acceleration Mode Enable */

/* This matches the IVB graphics documentation, not the IVB CPU documentation */
#define GMS_32MB                        0x05
#define GMS_48MB                        0x06
#define GMS_64MB                        0x07
#define GMS_128MB                       0x08
#define GMS_256MB                       0x09
#define GMS_96MB                        0x0A
#define GMS_160MB                       0x0B
#define GMS_224MB                       0x0C
#define GMS_352MB                       0x0D
#define GMS_0MB                         0x00
#define GMS_32MB_1                      0x01
#define GMS_64MB_1                      0x02
#define GMS_96MB_1                      0x03
#define GMS_128MB_1                     0x04
#define GMS_448MB                       0x0E
#define GMS_480MB                       0x0F
#define GMS_512MB                       0x10

#define GGMS_None                       0x00
#define GGMS_1MB                        0x01
#define GGMS_2MB                        0x02

/* 1.27 BDSM - Base Data of Stolen Memory */

#define BDSM                            0x5C    /* In PCI Config Space */

#define BDSM_LOCK                       (1 << 0)
#define BDSM_ADDR_MASK                  (0xfff << 20)

/* Vol 3. Part 3. North Display Engine Registers */
/* 3.1.1 VGA Control */

#define VGA_CONTROL                     0x41000 /* R/W */

#define VGA_DISABLE                     (1 << 31)

/* 3.7.1 ARB_CTL-Display Arbitration Control 1 */

#define ARB_CTL                         0x45000 /* R/W */

#define ARB_CTL_HP_DATA_REQUEST_LIMIT_MASK          0x7f
#define ARB_CTL_HP_PAGE_BREAK_LIMIT_SHIFT           8
#define ARB_CTL_HP_PAGE_BREAK_LIMIT_MASK            0x1f
#define ARB_CTL_TILED_ADDRESS_SWIZZLING             (1 << 13)
#define ARB_CTL_TLB_REQUEST_IN_FLIGHT_LIMIT_SHIFT   16
#define ARB_CTL_TLB_REQUEST_IN_FLIGHT_LIMIT_MASK    0x7
#define ARB_CTL_TLB_REQUEST_LIMIT_SHIFT             20
#define ARB_CTL_TLB_REQUEST_LIMIT_MASK              0x7
#define ARB_CTL_LP_WRITE_REQUEST_LIMIT_SHIFT        24
#define ARB_CTL_LP_WRITE_REQUEST_LIMIT_MASK         0x3
#define ARB_CTL_HP_QUEUE_WATERMARK_SHIFT            26
#define ARB_CTL_HP_QUEUE_WATERMARK_MASK             0x7

/* 2.16.2-3 Address Decode Channel Registers */

#define MAD_DIMM_CH0                    0x5004
#define MAD_DIMM_CH1                    0x5008

#define MAD_DIMM_A_SIZE_SHIFT           0
#define MAD_DIMM_A_SIZE_MASK            0xff
#define MAD_DIMM_B_SIZE_SHIFT           8
#define MAD_DIMM_B_SIZE_MASK            0xff
#define MAD_DIMM_AB_SIZE_MASK           0xffff
#define MAD_DIMM_A_SELECT               (1 << 16)
#define MAD_DIMM_A_DUAL_RANK            (1 << 17)
#define MAD_DIMM_B_DUAL_RANK            (1 << 18)
#define MAD_DIMM_A_X16                  (1 << 19)
#define MAD_DIMM_B_X16                  (1 << 20)
#define MAD_DIMM_RANK_INTERLEAVE        (1 << 21)
#define MAD_DIMM_ENH_INTERLEAVE         (1 << 22)
#define MAD_DIMM_ECC_MODE               (3 << 24)

/* Force Wake */

#define ECOBUS                          0xA180
#define FORCE_WAKE_MT                   0xA188
#define FORCE_WAKE                      0xA18C
/* GTSP1, Address: 130044h-130047h
 * 15:0 Multiple Force Wake GT programs this field with the multiple force wake
 * status. Software reads this field to find the status. Refer to MULTIFORCEWAKE
 * 0xA188 register description for the usage.
 */
#define FORCE_WAKE_MT_ACK               0x130044
/* Intel® Open Source HD Graphics, Intel Iris™ Graphics, and Intel Iris™ Pro
 * Graphics
 * Volume 2c: Command Reference: Registers
 * Part 1 – Registers A through L
 * GTFORCEAWAKE, P941, Address: 130090h-130093h
 * This field is no longer used. The multiple force wake mechanism has replaced
 * it. Refer to MULTIFORCEWAKE 0xA188 register description for the usage.
 */
#define FORCE_WAKE_ACK                  0x130090

/* Tile Ctrl - control register for cpu gtt access */

#define TILE_CTL                         0x101000       /* R/W */

#define TILE_CTL_SWIZZLE                (1 << 0)
#define TILE_CTL_TLB_PREFETCH_DISABLE   (1 << 2)
#define TILE_CTL_BACKSNOOP_DISABLE      (1 << 3)

/* Display Engine Registers */

/* DPLL (Display PLL) Control */
#define DPLL_A_CTRL                     0x06014
#define DPLL_B_CTRL                     0x06018

#define DPLL_VCO_ENABLE                 (1 << 31)
#define DPLL_SDVO_HIGH_SPEED            (1 << 30)
#define DPLL_DVO_2X_MODE                (1 << 30)
#define DPLL_EXT_BUFFER_ENABLE_VLV      (1 << 30)
#define DPLL_SYNCLOCK_ENABLE            (1 << 29)
#define DPLL_REF_CLK_ENABLE_VLV         (1 << 29)
#define DPLL_VGA_MODE_DIS               (1 << 28)
#define DPLL_SDVO_PHASE_SELECT          (1 << 20)

/* Power Management */
#define GEN6_RC_STATE                   0x0A094
#define GEN6_RC1_WAKE_RATE_LIMIT        0x0A098
#define GEN6_RC6_WAKE_RATE_LIMIT        0x0A09C
#define GEN6_RC6pp_WAKE_RATE_LIMIT      0x0A0A0
#define GEN6_RC_EVALUATION_INTERVAL     0x0A0A8
#define GEN6_RC_IDLE_HYSTERSIS          0x0A0AC

/* GPU Frequency Control */
#define GEN6_RPNSWREQ                   0x0A008
/* RPNSWREQ: bit 31 = SW freq req enable, bits 23:16 = freq in 50MHz units */
#define GEN6_TURBO_DISABLE              (1u << 31)      /* SW request enable */
#define GEN6_FREQ_SHIFT                 16              /* bits 23:16 */
#define GEN6_FREQ_MASK                  (0xFFu << 16)

#define GEN6_RC_VIDEO_FREQ              0x0A00C
#define GEN6_RP_DOWN_TIMEOUT            0x0A010
#define GEN6_RP_INTERRUPT_LIMITS        0x0A014
#define GEN6_RPSTAT1                    0x0A01C
/* RPSTAT1 on Skylake Gen9: current freq in bits 31:24, in 50MHz units */
#define GEN6_CAGF_SHIFT                 24
#define GEN6_CAGF_MASK                  (0xFFu << 24)
#define GEN6_RP_CONTROL                 0x0A024

/* RC6 Power Control */
#define GEN6_RC_CONTROL                 0x0A090
#define GEN6_RC_CTL_RC6_ENABLE          (1 << 18)
#define GEN6_RC_CTL_EI_MODE(x)          ((x) << 27)

/* Memory Bandwidth Monitoring */
#define GEN7_MCHBAR_MIRROR_BASE         0x140000
#define GEN7_MCHBAR_MIRROR_BASE_SNB     0x180000

/* GT Performance Monitoring */
#define GT_PERF_STATUS                  0x0A000
#define GT_PERF_LIMIT_REASONS           0x0A004

/* Display Port */
#define DP_A                            0x64000
#define DP_B                            0x64100
#define DP_C                            0x64200
#define DP_D                            0x64300

#define DP_PORT_EN                      (1 << 31)
#define DP_PIPEB_SELECT                 (1 << 30)
#define DP_PIPE_MASK                    (1 << 30)
#define DP_PIPE_SELECT_CHV(pipe)        ((pipe) << 16)
#define DP_PIPE_MASK_CHV                (3 << 16)

/* HDMI/DVI */
#define HDMIB                           0x61140
#define HDMIC                           0x61160
#define HDMID                           0x6116C

#define HDMI_PORT_EN                    (1 << 31)
#define HDMI_PIPEB_SELECT               (1 << 30)
#define HDMI_COLOR_RANGE_16_235         (1 << 8)
#define HDMI_AUDIO_ENABLE               (1 << 6)

/* Pipe Configuration */
#define PIPEACONF                       0x70008
#define PIPEBCONF                       0x71008
#define PIPECCONF                       0x72008

#define PIPE_ENABLE                     (1 << 31)
#define PIPE_STATE                      (1 << 30)
#define PIPE_PROGRESSIVE                (0 << 21)
#define PIPE_INTERLACED                 (1 << 21)

/* Plane Control */
#define DSPACNTR                        0x70180
#define DSPBCNTR                        0x71180
#define DSPCCNTR                        0x72180

#define DISPLAY_PLANE_ENABLE            (1 << 31)
#define DISPLAY_PLANE_DISABLE           0
#define DISPPLANE_GAMMA_ENABLE          (1 << 30)
#define DISPPLANE_PIXFORMAT_MASK        (0xf << 26)
#define DISPPLANE_8BPP                  (0x2 << 26)
#define DISPPLANE_BGRX555               (0x3 << 26)
#define DISPPLANE_BGRX565               (0x4 << 26)
#define DISPPLANE_BGRX888               (0x6 << 26)
#define DISPPLANE_RGBX888               (0xe << 26)

/* Cursor Control */
#define CURACNTR                        0x70080
#define CURBCNTR                        0x71080
#define CURCCNTR                        0x72080

#define CURSOR_MODE                     0x27
#define CURSOR_MODE_DISABLE             0x00
#define CURSOR_MODE_128_32B_AX          0x02
#define CURSOR_MODE_256_32B_AX          0x03
#define CURSOR_MODE_64_32B_AX           0x07
#define CURSOR_MODE_128_ARGB_AX         0x20
#define CURSOR_MODE_256_ARGB_AX         0x21
#define CURSOR_MODE_64_ARGB_AX          0x27
#define MCURSOR_PIPE_SELECT             (1 << 28)
#define MCURSOR_GAMMA_ENABLE            (1 << 26)

/* Interrupt Control */
#define DEISR                           0x44000
#define DEIMR                           0x44004
#define DEIIR                           0x44008
#define DEIER                           0x4400C

#define DE_MASTER_IRQ_CONTROL           (1 << 31)
#define DE_PCH_EVENT                    (1 << 28)
#define DE_PIPEC_VBLANK                 (1 << 10)
#define DE_PIPEB_VBLANK                 (1 << 5)
#define DE_PIPEA_VBLANK                 (1 << 0)

/* Cursor Registers */
#define CURABASE                        0x70084
#define CURBBASE                        0x71084
#define CURCBASE                        0x72084

#define CURAPOS                         0x70088
#define CURBPOS                         0x71088
#define CURCPOS                         0x72088

#define CURSOR_POS_SIGN_X               (1 << 15)
#define CURSOR_POS_SIGN_Y               (1 << 31)

/* Skylake Gen9 Display — Power Wells */
#define PWR_WELL_CTL2                   0x45404
#define PWR_WELL_MISC_IO_REQ            (1u << 31)
#define PWR_WELL_MISC_IO_STATE          (1u << 30)
#define PWR_WELL_DDI_A_REQ              (1 << 3)
#define PWR_WELL_DDI_A_STATE            (1 << 2)

/* Skylake Gen9 Display — CDCLK / DPLL */
#define LCPLL1_CTL                      0x46010
#define LCPLL1_PLL_ENABLE               (1u << 31)
#define LCPLL1_PLL_LOCK                 (1u << 30)

/* DPLL_CTRL1/2 replace Gen6 DPLL_A/B on Skylake */
#define DPLL_CTRL1                      0x6C058
#define DPLL_CTRL1_LINK_RATE_2700       0   /* HBR2 */
#define DPLL_CTRL1_LINK_RATE_1350       1   /* HBR  */
#define DPLL_CTRL1_LINK_RATE_810        2   /* RBR  */
#define DPLL_CTRL1_OVERRIDE(n)          (1 << ((n) * 6))

#define DPLL_CTRL2                      0x6C05C
#define DPLL_CTRL2_DDI_CLK_OFF(port)    (1 << ((port) * 3 + 15))
#define DPLL_CTRL2_DDI_SEL_SHIFT(port)  ((port) * 3 + 1)
#define DPLL_CTRL2_DDI_SEL_DPLL0        0
#define DPLL_CTRL2_DDI_SEL_DPLL1        1
#define DPLL_CTRL2_DDI_CLK_OVERRIDE(port) (1 << ((port) * 3))

/* Skylake Gen9 Display — DDI Buffer Control (port A = eDP) */
#define DDI_BUF_CTL_A                   0x64000
#define DDI_BUF_CTL_ENABLE              (1u << 31)
#define DDI_BUF_CTL_PORT_WIDTH_X1       (0 << 1)
#define DDI_BUF_CTL_PORT_WIDTH_X2       (1 << 1)
#define DDI_BUF_CTL_PORT_WIDTH_X4       (3 << 1)
#define DDI_BUF_CTL_PORT_WIDTH_MASK     (7 << 1)
#define DDI_BUF_CTL_IDLE                (1 << 7)

/* DDI AUX Channel (port A) */
#define DDI_AUX_CTL_A                   0x64010
#define DDI_AUX_CTL_SEND_BUSY           (1u << 31)
#define DDI_AUX_CTL_DONE                (1 << 30)
#define DDI_AUX_CTL_TIMEOUT             (1 << 26)
#define DDI_AUX_CTL_MSG_SIZE(n)         ((n) << 20)
#define DDI_AUX_DATA_A0                 0x64014

/* Skylake Gen9 Display — Transcoder DDI Function Control */
#define TRANS_DDI_FUNC_CTL_A            0x60400
#define TRANS_DDI_FUNC_CTL_B            0x61400
#define TRANS_DDI_FUNC_CTL_C            0x62400
#define TRANS_DDI_FUNC_ENABLE           (1u << 31)
#define TRANS_DDI_SELECT_NONE           (0 << 28)
#define TRANS_DDI_SELECT_DDI_B          (1 << 28)
#define TRANS_DDI_SELECT_DDI_C          (2 << 28)
#define TRANS_DDI_SELECT_DDI_D          (3 << 28)
#define TRANS_DDI_SELECT_DDI_A          (4 << 28)
#define TRANS_DDI_SELECT_MASK           (7 << 28)
#define TRANS_DDI_MODE_HDMI             (0 << 24)
#define TRANS_DDI_MODE_DVI              (1 << 24)
#define TRANS_DDI_MODE_DP_SST           (2 << 24)
#define TRANS_DDI_MODE_DP_MST           (3 << 24)
#define TRANS_DDI_MODE_MASK             (7 << 24)
#define TRANS_DDI_BPC_8                 (0 << 20)
#define TRANS_DDI_BPC_10                (1 << 20)
#define TRANS_DDI_BPC_6                 (2 << 20)
#define TRANS_DDI_BPC_12                (3 << 20)
#define TRANS_DDI_PORT_WIDTH_X1         (0 << 1)
#define TRANS_DDI_PORT_WIDTH_X2         (1 << 1)
#define TRANS_DDI_PORT_WIDTH_X4         (3 << 1)
#define TRANS_DDI_PORT_WIDTH_MASK       (7 << 1)

/* Skylake Gen9 Display — eDP Panel Power Sequencing */
#define PP_STATUS                       0xC7200
#define PP_STATUS_ON                    (1u << 31)
#define PP_STATUS_SEQUENCE_MASK         (3 << 28)
#define PP_STATUS_SEQUENCE_NONE         (0 << 28)

#define PP_CONTROL                      0xC7204
#define PP_CONTROL_VDD_FORCE            (1 << 3)
#define PP_CONTROL_BACKLIGHT_ENABLE     (1 << 2)
#define PP_CONTROL_POWER_DOWN_RST       (1 << 1)
#define PP_CONTROL_POWER_STATE          (1 << 0)

#define PP_ON_DELAYS                    0xC7208
#define PP_OFF_DELAYS                   0xC720C
#define PP_DIVISOR                      0xC7210

/* eDP PSR (Panel Self Refresh) — disable for initial bringup */
#define EDP_PSR_CTL                     0x64900
#define EDP_PSR_ENABLE                  (1u << 31)

/* Backlight PWM */
#define BLC_PWM_CTL2                    0x48250
#define BLC_PWM_CTL                     0x48254
#define BLC_PWM_CTL_ENABLE              (1u << 31)

/* Skylake Gen9 Universal Plane A (pipe A, plane 1) */
#define PLANE_CTL_1_A                   0x70180   /* same offset as DSPACNTR */
#define PLANE_CTL_ENABLE                (1u << 31)
#define PLANE_CTL_FORMAT_XRGB           (4u << 24) /* 32bpp XRGB/BGRX linear */
#define PLANE_CTL_TILED_LINEAR          (0u << 10)

#define PLANE_STRIDE_1_A                0x70188   /* stride in 64-byte units */
#define PLANE_POS_1_A                   0x7018C   /* (y<<16)|x */
#define PLANE_SIZE_1_A                  0x70190   /* (h-1)<<16|(w-1) */
#define PLANE_SURF_1_A                  0x7019C   /* surface GPU addr, triggers flip */

/* Pipe A source size (active area): (width-1)<<16 | (height-1) */
#define PIPEASRC                        0x6001C
