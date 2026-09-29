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
#include <stddef.h>

#include <string.h>

#include <device/display/fb.h>
#include <lib/kmalloc.h>

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

#define SPLASH_BG       0x0E1A24
#define SPLASH_FG       0x4FC7D6
#define SPLASH_TRACK    0x1C2E3A

static int splash_track_x, splash_track_y, splash_track_w, splash_track_h;

static void splash_rect(fb_info_t * fb, int x, int y, int w, int h,
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
    if (x + w > (int) fb->width)
        w = (int) fb->width - x;
    if (y + h > (int) fb->height)
        h = (int) fb->height - y;
    if (w <= 0 || h <= 0)
        return;

    for (int j = 0; j < h; j++) {
        uint32_t *row =
            (uint32_t *) (fb->backbuffer + (uint64_t) fb->pitch * (y + j));
        for (int i = 0; i < w; i++)
            row[x + i] = color;
    }
}

/* Thick line drawn as overlapping t-by-t squares. */
static void splash_line(fb_info_t * fb, int x0, int y0, int x1, int y1, int t,
                        uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = (dx < 0 ? -dx : dx);
    if ((dy < 0 ? -dy : dy) > steps)
        steps = (dy < 0 ? -dy : dy);
    if (steps == 0)
        steps = 1;

    for (int s = 0; s <= steps; s++)
        splash_rect(fb, x0 + dx * s / steps - t / 2,
                    y0 + dy * s / steps - t / 2, t, t, color);
}

void fb_splash(fb_info_t * fb)
{
    splash_rect(fb, 0, 0, (int) fb->width, (int) fb->height, SPLASH_BG);

    int H = (int) fb->height / 4;
    int W = H * 3 / 5;
    int gap = W / 3;
    int t = W / 4;
    int total = W * 3 + gap * 2;
    int ox = ((int) fb->width - total) / 2;
    int oy = ((int) fb->height - H) / 2 - (int) fb->height / 12;

    /* H */
    splash_rect(fb, ox, oy, t, H, SPLASH_FG);
    splash_rect(fb, ox + W - t, oy, t, H, SPLASH_FG);
    splash_rect(fb, ox, oy + (H - t) / 2, W, t, SPLASH_FG);

    /* N */
    int nx = ox + W + gap;
    splash_rect(fb, nx, oy, t, H, SPLASH_FG);
    splash_rect(fb, nx + W - t, oy, t, H, SPLASH_FG);
    splash_line(fb, nx + t, oy, nx + W - t, oy + H, t, SPLASH_FG);

    /* K */
    int kx = nx + W + gap;
    splash_rect(fb, kx, oy, t, H, SPLASH_FG);
    splash_line(fb, kx + t, oy + H / 2, kx + W, oy, t, SPLASH_FG);
    splash_line(fb, kx + t, oy + H / 2, kx + W, oy + H, t, SPLASH_FG);

    splash_track_w = (int) fb->width / 3;
    splash_track_h = (int) fb->height / 100;
    if (splash_track_h < 4)
        splash_track_h = 4;
    splash_track_x = ((int) fb->width - splash_track_w) / 2;
    splash_track_y = (int) fb->height * 3 / 4;
    splash_rect(fb, splash_track_x, splash_track_y, splash_track_w,
                splash_track_h, SPLASH_TRACK);

    fb_refresh(fb);
}

void fb_splash_progress(fb_info_t * fb, uint32_t percent)
{
    if (percent > 100)
        percent = 100;

    splash_rect(fb, splash_track_x, splash_track_y,
                splash_track_w * (int) percent / 100, splash_track_h,
                SPLASH_FG);
    fb_refresh(fb);
}
