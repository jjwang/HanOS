/**-----------------------------------------------------------------------------

 @file    font.h
 @brief   Definition of font data matrix
 @details
 @verbatim

  It contains font data by static defined matrix. The ASC16 and HZK16 were
  included.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04

#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_MODEHASSEQ 0x04
#define PSF1_MAXMODE    0x05

#define PSF1_SEPARATOR  0xFFFF
#define PSF1_STARTSEQ   0xFFFE

typedef struct [[gnu::packed]] {
    uint8_t magic[2];       /* Magic number */
    uint8_t mode;           /* PSF font mode */
    uint8_t charsize;       /* Character size */
    uint8_t data[];         /* Actual font data */
} font_psf1_t;

#define PSF2_MAGIC0     0x72
#define PSF2_MAGIC1     0xb5
#define PSF2_MAGIC2     0x4a
#define PSF2_MAGIC3     0x86

/* bits used in flags */
#define PSF2_HAS_UNICODE_TABLE 0x01

/* max version recognized so far */
#define PSF2_MAXVERSION 0

/* UTF8 separators */
#define PSF2_SEPARATOR  0xFF
#define PSF2_STARTSEQ   0xFE

typedef struct [[gnu::packed]] {
    uint32_t magic;         /* Magic bytes to identify PSF */
    uint32_t version;       /* Zero */
    uint32_t headersize;    /* Offset of bitmaps in file, 32 */
    uint32_t flags;         /* 0 if there's no unicode table */

    uint32_t numglyph;      /* Number of glyphs */
    uint32_t glyph_size;    /* Size of each glyph */
    uint32_t height;        /* Height in pixels */
    uint32_t width;         /* Width in pixels */

    uint8_t data[];         /* Actual font data */
} font_psf2_t;

