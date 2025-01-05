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

#define ARB_MODE                        0x04030     /* R/W */

#define ARB_MODE_GGTAGDR                (1 << 0)    /* GTT Accesses GDR */
#define ARB_MODE_CCGDREN                (1 << 1)    /* Color Cache GDR Enable Bit */
#define ARB_MODE_DCGDREN                (1 << 2)    /* Depth Cache GDR Enable Bit */
#define ARB_MODE_TCGDREN                (1 << 3)    /* Texture Cache GDR Enable Bit */
#define ARB_MODE_VMC_GDR_EN             (1 << 4)    /* VMC GDR Enable */
#define ARB_MODE_AS4TS                  (1 << 5)    /* Address Swizzling for Tiled Surfaces */
#define ARB_MODE_CDPS                   (1 << 8)    /* Color/Depth Port Share Bit */
#define ARB_MODE_GAMPD_GDR              (1 << 9)    /* GAM PD GDR */
#define ARB_MODE_BLB_GDR                (1 << 10)   /* BLB GDR */
#define ARB_MODE_STC_GDR                (1 << 11)   /* STC GDR */
#define ARB_MODE_HIZ_GDR                (1 << 12)   /* HIZ GDR */
#define ARB_MODE_DC_GDR                 (1 << 13)   /* DC GDR */
#define ARB_MODE_GAM2BGTTT              (1 << 14)   /* GAM to Bypass GTT Translation */

/* Vol 3. Part 1. VGA and Extended VGA Registers */
/* 1.2.1 Sequencer Index */

#define SR_INDEX                        0x3c4
#define SR_DATA                         0x3c5

/* 1.2.3 Clocking Mode */

#define SEQ_CLOCKING                    0x01
#define SCREEN_OFF                      (1 << 5)

/* Vol 3. Part 2. PCI Registers */
/* 1.25 MGGC0 - Mirror of GMCH Graphics Control Register */

#define MGGC0                           0x50        /* In PCI Config Space */

#define GGC_LOCK                        (1 << 0)
#define GGC_IVD                         (1 << 1)    /* IGD VGA Disable */
#define GGC_GMS_SHIFT                   3           /* Graphics Mode Select */
#define GGC_GMS_MASK                    0x1f
#define GGC_GGMS_SHIFT                  8           /* GTT Graphics Memory Size */
#define GGC_GGMS_MASK                   0x3
#define GGC_VAMEN                       (1 << 14)   /* Versatile Acceleration Mode Enable */

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

#define BDSM                            0x5C /* In PCI Config Space */

#define BDSM_LOCK                       (1 << 0)
#define BDSM_ADDR_MASK                  (0xfff << 20)

/* Vol 3. Part 3. North Display Engine Registers */
/* 3.1.1 VGA Control */

#define VGA_CONTROL                     0x41000     /* R/W */

#define VGA_DISABLE                     (1 << 31)

/* 3.7.1 ARB_CTL-Display Arbitration Control 1 */

#define ARB_CTL                         0x45000     /* R/W */

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

#define TILE_CTL                         0x101000     /* R/W */

#define TILE_CTL_SWIZZLE                (1 << 0)
#define TILE_CTL_TLB_PREFETCH_DISABLE   (1 << 2)
#define TILE_CTL_BACKSNOOP_DISABLE      (1 << 3) 
