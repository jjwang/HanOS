/**-----------------------------------------------------------------------------

 @file    gpu_cursor.c
 @brief   Hardware cursor surface for the userspace GPU server

 @endverbatim
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <gpu/gfx.h>
#include <gpu_reg.h>
#include <gpu/gpu_log.h>

/* 64x64 ARGB cursor surface. The tip is the hotspot at (x, y). */
#define CURSOR_SIZE     64

static struct {
    gfx_pci_t *pci;
    gfx_object_t obj;
    uint32_t width;
    uint32_t height;
    int32_t x;
    int32_t y;
    bool ready;
} cursor = { 0 };

/* The Linux default pointer (the DMZ-White `left_ptr`): a white arrow with a
 * black outline, 24x24 ARGB with an anti-aliased edge, hotspot (7,4). Taken
 * from the installed cursor theme. */
#define CURSOR_IMG_W    24
#define CURSOR_IMG_H    24
#define CURSOR_HOT_X    7
#define CURSOR_HOT_Y    4

static const uint32_t cursor_image[CURSOR_IMG_H][CURSOR_IMG_W] = {
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000000,
      0x02000000, 0x01000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x04000000,
      0x87000000, 0x0c000000, 0x02000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x09000000,
      0xff000000, 0x99030303, 0x13000000, 0x02000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xf9151515, 0x95020202, 0x12000000, 0x02000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffdcdcdc, 0xfa111111, 0x8b010101, 0x10000000, 0x02000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfffefefe, 0xffd6d6d6, 0xfa0e0e0e, 0x81010101, 0x0f000000,
      0x02000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfffbfbfb, 0xffffffff, 0xffd0d0d0, 0xfa0c0c0c, 0x78000000,
      0x0e000000, 0x02000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfff7f7f7, 0xfffefefe, 0xffffffff, 0xffc9c9c9, 0xfa0a0a0a,
      0x70000000, 0x0d000000, 0x01000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfff4f4f4, 0xfffbfbfb, 0xffffffff, 0xffffffff, 0xffc3c3c3,
      0xfa0a0a0a, 0x68000000, 0x0c000000, 0x01000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfff0f0f0, 0xfff8f8f8, 0xffffffff, 0xffffffff, 0xffffffff,
      0xffbbbbbb, 0xf9090909, 0x60000000, 0x0b000000, 0x01000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffededed, 0xfff4f4f4, 0xfffbfbfb, 0xffffffff, 0xffffffff,
      0xffffffff, 0xffb4b4b4, 0xf8090909, 0x59000000, 0x0b000000, 0x01000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffe9e9e9, 0xfff1f1f1, 0xfff8f8f8, 0xffffffff, 0xffffffff,
      0xffffffff, 0xffffffff, 0xffaaaaaa, 0xf7090909, 0x51000000, 0x09000000,
      0x01000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffe6e6e6, 0xffededed, 0xfff4f4f4, 0xfffbfbfb, 0xffffffff,
      0xffffffff, 0xffffffff, 0xffffffff, 0xfea2a2a2, 0xf5090909, 0x4b000000,
      0x08000000, 0x01000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffe3e3e3, 0xffeaeaea, 0xfff1f1f1, 0xfff8f8f8, 0xfffdfdfd,
      0xfd2b2b2b, 0xfc080808, 0xfc080808, 0xfc080808, 0xff010101, 0xee020202,
      0x3a000000, 0x04000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffdfdfdf, 0xffe6e6e6, 0xffd7d7d7, 0xff9c9c9c, 0xfffcfcfc,
      0xfe909090, 0xdf0a0a0a, 0x6b000000, 0x5c000000, 0x59000000, 0x53000000,
      0x31000000, 0x08000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffdcdcdc, 0xffcbcbcb, 0xfa1a1a1a, 0xfb0b0b0b, 0xffebebeb,
      0xfff5f5f5, 0xfa0e0e0e, 0x67000000, 0x1a000000, 0x0f000000, 0x0e000000,
      0x09000000, 0x02000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xffc0c0c0, 0xfa161616, 0xb5040404, 0xcf080808, 0xfd7e7e7e,
      0xfffcfcfc, 0xfd7f7f7f, 0xc8090909, 0x1b000000, 0x02000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xff000000, 0xfa141414, 0xae030303, 0x45000000, 0x4e000000, 0xf90c0c0c,
      0xffededed, 0xffefefef, 0xfa0a0a0a, 0x4d000000, 0x09000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x0b000000,
      0xfe000000, 0xa4020202, 0x41000000, 0x13000000, 0x0d000000, 0xc4090909,
      0xfd818181, 0xfffcfcfc, 0xf8393939, 0x89000000, 0x12000000, 0x01000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x09000000,
      0x7e000000, 0x35000000, 0x12000000, 0x02000000, 0x03000000, 0x42000000,
      0xf8080808, 0xf83e3e3e, 0xf50a0a0a, 0x6e000000, 0x11000000, 0x01000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x03000000,
      0x0e000000, 0x0b000000, 0x02000000, 0x00000000, 0x00000000, 0x08000000,
      0x4c000000, 0x90000000, 0x65000000, 0x28000000, 0x07000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x01000000,
      0x02000000, 0x01000000, 0x00000000, 0x00000000, 0x00000000, 0x01000000,
      0x07000000, 0x12000000, 0x11000000, 0x06000000, 0x01000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
    { 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
      0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000 },
};

/* Copy the cursor image into the top-left of the 64x64 ARGB surface. */
static void cursor_draw(volatile uint8_t * base)
{
    memset((void *) base, 0, CURSOR_SIZE * CURSOR_SIZE * 4);

    for (uint32_t y = 0; y < CURSOR_IMG_H; y++) {
        volatile uint32_t *row =
            (volatile uint32_t *) (base + y * CURSOR_SIZE * 4);

        for (uint32_t x = 0; x < CURSOR_IMG_W; x++)
            row[x] = cursor_image[y][x];
    }
}

static uint32_t cursor_pos(int32_t x, int32_t y)
{
    uint32_t pos = 0;

    if (x < 0) {
        pos |= CURSOR_POS_SIGN_X;
        x = -x;
    }
    if (y < 0) {
        pos |= CURSOR_POS_SIGN_Y;
        y = -y;
    }
    return pos | ((uint32_t) x & CURSOR_POS_X_MASK)
        | (((uint32_t) y & CURSOR_POS_Y_MASK) << CURSOR_POS_Y_SHIFT);
}

bool skl_cursor_init(gfx_pci_t * pci, gfx_mem_manager_t * mgr, gfx_gtt_t * gtt,
                     uint32_t width, uint32_t height)
{
    uint32_t size = CURSOR_SIZE * CURSOR_SIZE * 4;

    if (cursor.ready)
        return true;

    if (!gfx_alloc(mgr, gtt, &cursor.obj, size, 64)) {
        kloge("GFX: cursor: allocation (%u bytes) failed\n", size);
        return false;
    }

    cursor_draw((volatile uint8_t *) cursor.obj.cpu_addr);
    gpu_flush((const void *)cursor.obj.cpu_addr, size);

    cursor.pci = pci;
    cursor.width = width;
    cursor.height = height;
    cursor.x = (int32_t) width / 2;
    cursor.y = (int32_t) height / 2;

    if (pci->gen9) {
        /* A small data-buffer allocation and watermark so the cursor fetch is
         * not starved; the primary plane uses blocks 0..445. */
        gfx_outd(pci, CUR_BUF_CFG, PLANE_BUF_END(477) | PLANE_BUF_START(446));
        for (uint32_t lvl = 0; lvl < 8; lvl++)
            gfx_outd(pci, CUR_WM_0 + lvl * 4,
                     PLANE_WM_EN | PLANE_WM_IGNORE_LINES
                     | PLANE_WM_BLOCKS(32));
        gfx_outd(pci, CUR_WM_TRANS,
                 PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(32));
    }

    /* Square 64x64 ARGB: FBC off, then control, position and finally base
     * (the CURBASE write arms the double-buffered cursor state). */
    gfx_outd(pci, CUR_FBC_CTL_A, 0);
    gfx_outd(pci, CURACNTR, CURSOR_MODE_64_ARGB_AX);
    gfx_outd(pci, CURAPOS,
             cursor_pos(cursor.x - CURSOR_HOT_X, cursor.y - CURSOR_HOT_Y));
    gfx_outd(pci, CURABASE, (uint32_t) cursor.obj.gfx_addr);

    cursor.ready = true;
    klogi("GFX: cursor: 64x64 ARGB at %d,%d (gpu 0x%08x)\n", cursor.x,
          cursor.y, (uint32_t) cursor.obj.gfx_addr);
    return true;
}

void skl_cursor_set(int32_t x, int32_t y)
{
    if (!cursor.ready)
        return;

    if (x < 0)
        x = 0;
    else if (x > (int32_t) cursor.width - 1)
        x = (int32_t) cursor.width - 1;
    if (y < 0)
        y = 0;
    else if (y > (int32_t) cursor.height - 1)
        y = (int32_t) cursor.height - 1;

    cursor.x = x;
    cursor.y = y;

    /* CURPOS then CURBASE: the base write arms the update. (x, y) is the
     * hotspot, so the surface top-left is offset by it. */
    gfx_outd(cursor.pci, CURAPOS,
             cursor_pos(cursor.x - CURSOR_HOT_X, cursor.y - CURSOR_HOT_Y));
    gfx_outd(cursor.pci, CURABASE, (uint32_t) cursor.obj.gfx_addr);
}

void skl_cursor_move(int32_t dx, int32_t dy)
{
    skl_cursor_set(cursor.x + dx, cursor.y + dy);
}
