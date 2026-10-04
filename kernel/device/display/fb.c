/**-----------------------------------------------------------------------------

 @file    fb.c
 @brief   Implementation of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer was got
  from Limine bootloader.

  The kernel keeps the framebuffer geometry and, once a private buffer is
  needed, a back buffer. Text rendering lives in the userspace console server.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stddef.h>

#include <string.h>
#include <version.h>

#include <device/display/fb.h>
#include <lib/kmalloc.h>
#include <printf.h>

static fb_info_t fb_global;

fb_info_t *fb_get(void)
{
    return &fb_global;
}

void fb_init(fb_info_t *fb, struct limine_framebuffer *s)
{
    if (s == NULL) {
        if ((uint64_t) fb->addr == (uint64_t) fb->backbuffer) {
            fb->backbuffer = kmalloc(fb->backbuffer_len);
            memcpy(fb->backbuffer, fb->addr, fb->backbuffer_len);
        }
        return;
    }

    fb->addr = (uint8_t *) s->address;
    fb->width = s->width;
    fb->height = s->height;
    fb->pitch = s->pitch;

    fb->backbuffer_len = fb->height * fb->pitch;
    fb->backbuffer = fb->addr;
}

/* Copy the frame to the scan-out with non-temporal stores. The display engine
 * samples DRAM, not the CPU cache, so a cached write only becomes visible when
 * its line evicts. More importantly, the kernel reaches the scan-out through
 * its cacheable direct map while the userspace console maps the same memory as
 * write-combining: a line left dirty by the kernel would later write back over
 * the console's frames. Non-temporal stores never allocate a cache line, so no
 * stale write-back can survive the hand-off. */
static void fb_blit_scanout(uint8_t * dst, const uint8_t * src, uint64_t len)
{
    uint64_t i = 0;

    for (; i + sizeof(uint64_t) <= len; i += sizeof(uint64_t)) {
        uint64_t v;
        memcpy(&v, src + i, sizeof(v));
        asm volatile ("movnti %1, (%0)"::"r"(dst + i), "r"(v):"memory");
    }
    for (; i < len; i++)
        dst[i] = src[i];
    asm volatile ("sfence":::"memory");
}

void fb_refresh(fb_info_t * fb)
{
    if ((uint64_t) fb->addr != (uint64_t) fb->backbuffer)
        fb_blit_scanout(fb->addr, fb->backbuffer, fb->backbuffer_len);
}

/* --- Boot splash --------------------------------------------------------- */

/**
 * @brief PSF1 bitmap font header followed by its glyph data
 *
 * Each glyph is 8 pixels wide and charsize bytes tall, one byte per row.
 */
typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
    uint8_t data[];
} psf1_font_t;

extern psf1_font_t boot_font_norm, boot_font_bold;

#define SPLASH_BG       0x000000
#define SPLASH_LOGO     0x00AAAA
#define SPLASH_DESC     0xAAAAAA
#define SPLASH_TRACK    0x1E1E1E
#define SPLASH_FILL     0x00AAAA
#define SPLASH_SCALE    6

static int32_t splash_track_x, splash_track_y, splash_track_w, splash_track_h;

static void splash_rect(fb_info_t * fb, int32_t x, int32_t y, int32_t w, int32_t h,
                        uint32_t color)
{
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > (int32_t) fb->width)
        w = (int32_t) fb->width - x;
    if (y + h > (int32_t) fb->height)
        h = (int32_t) fb->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int32_t j = 0; j < h; j++) {
        uint32_t *row =
            (uint32_t *) (fb->backbuffer + (uint64_t) fb->pitch * (y + j));
        for (int32_t i = 0; i < w; i++)
            row[x + i] = color;
    }
}

static void splash_glyph(fb_info_t * fb, const psf1_font_t * font, int32_t x,
                         int32_t y, uint8_t ch, int32_t scale, uint32_t color)
{
    const uint8_t *glyph = font->data + (uint32_t) ch * font->charsize;

    for (int32_t i = 0; i < font->charsize; i++) {
        uint8_t bits = glyph[i];
        for (int32_t k = 0; k < 8; k++)
            if (bits & (0x80 >> k))
                splash_rect(fb, x + k * scale, y + i * scale, scale, scale,
                            color);
    }
}

static void splash_text(fb_info_t * fb, const psf1_font_t * font, int32_t x, int32_t y,
                        const char *s, int32_t scale, uint32_t color)
{
    for (; *s != '\0'; s++, x += 8 * scale)
        splash_glyph(fb, font, x, y, (uint8_t) *s, scale, color);
}

void fb_splash(fb_info_t * fb)
{
    splash_rect(fb, 0, 0, (int32_t) fb->width, (int32_t) fb->height, SPLASH_BG);

    const char *logo = "HNK";
    int32_t scale = SPLASH_SCALE;
    int32_t lw = (int32_t) strlen(logo) * 8 * scale;
    int32_t lx = ((int32_t) fb->width - lw) / 2;
    int32_t ly = ((int32_t) fb->height - boot_font_bold.charsize * scale) / 2
        - (int32_t) fb->height / 8;
    splash_text(fb, &boot_font_bold, lx, ly, logo, scale, SPLASH_LOGO);

    char desc[96];
    snprintf(desc, sizeof(desc),
             "- Microkernel-based General Purpose OS Kernel for x86-64 v%s -",
             VERSION);
    int32_t dx = ((int32_t) fb->width - (int32_t) strlen(desc) * 8) / 2;
    int32_t dy = ((int32_t) fb->height + boot_font_bold.charsize * scale) / 2
        - (int32_t) fb->height / 8;
    splash_text(fb, &boot_font_norm, dx, dy, desc, 1, SPLASH_DESC);

    splash_track_w = (int32_t) fb->width / 4;
    splash_track_h = 6;
    splash_track_x = ((int32_t) fb->width - splash_track_w) / 2;
    splash_track_y = (int32_t) fb->height * 2 / 3;
    splash_rect(fb, splash_track_x, splash_track_y, splash_track_w,
                splash_track_h, SPLASH_TRACK);

    fb_refresh(fb);
}

void fb_splash_progress(fb_info_t * fb, uint32_t percent)
{
    if (percent > 100)
        percent = 100;

    splash_rect(fb, splash_track_x, splash_track_y,
                splash_track_w * (int32_t) percent / 100, splash_track_h,
                SPLASH_FILL);
    fb_refresh(fb);
}
