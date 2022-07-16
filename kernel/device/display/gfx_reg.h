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

/* Registers not in the Spec (Found in Linux Driver) */
/* Force Wake */
/* Bring the card out of D6 state */

#define ECOBUS                          0xA180
#define FORCE_WAKE_MT                   0xA188 
#define FORCE_WAKE                      0xA18C 
#define FORCE_WAKE_MT_ACK               0x130040 
#define FORCE_WAKE_ACK                  0x130090 
