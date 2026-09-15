/**-----------------------------------------------------------------------------

 @file    console.c
 @brief   Userspace framebuffer console server
 @details
 @verbatim

   The kernel grants this server the framebuffer mapping and an endpoint for
   console bytes. The server keeps a back buffer, renders text with the shared
   gohufont glyphs and blits it to the framebuffer after draining its queue.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
    uint8_t data[];
} psf1_t;

extern psf1_t term_font_norm;
extern psf1_t term_font_bold;

#define FONT_W      8
#define FONT_H      15

#define COLOR_BLACK     0x000000
#define COLOR_RED       0xAA0000
#define COLOR_GREEN     0x00AA00
#define COLOR_YELLOW    0xAAAA00
#define COLOR_BLUE      0x0000AA
#define COLOR_MAGENTA   0xAA00AA
#define COLOR_CYAN      0x00AAAA
#define COLOR_GREY      0xAAAAAA

static uint8_t *fbio;
static uint8_t *back;
static uint32_t fb_w;
static uint32_t fb_h;
static uint32_t fb_pitch;
static uint32_t cols;
static uint32_t rows;
static uint32_t cx;
static uint32_t cy;
static uint32_t fg = COLOR_GREY;
static uint32_t bg = COLOR_BLACK;

/* Rows changed since the last blit, so only those are copied to the
 * framebuffer instead of the whole screen. */
static int dirty_min = -1;
static int dirty_max = -1;

static void mark_row(uint32_t y)
{
    if (dirty_min < 0 || (int) y < dirty_min)
        dirty_min = (int) y;
    if (dirty_max < 0 || (int) y > dirty_max)
        dirty_max = (int) y;
}

static void mark_all(void)
{
    dirty_min = 0;
    dirty_max = (int) fb_h - 1;
}

static void flush(void)
{
    if (dirty_min < 0)
        return;

    uint64_t off = (uint64_t) dirty_min * fb_pitch;
    uint64_t len = (uint64_t) (dirty_max - dirty_min + 1) * fb_pitch;
    memcpy(fbio + off, back + off, len);

    dirty_min = -1;
    dirty_max = -1;
}

static void putpixel(uint32_t x, uint32_t y, uint32_t c)
{
    if (x >= fb_w || y >= fb_h)
        return;
    *(uint32_t *) (back + (uint64_t) y * fb_pitch + (uint64_t) x * 4) = c;
}

static void draw_cell(uint32_t col, uint32_t row, uint8_t ch)
{
    psf1_t *f = &term_font_norm;
    uint32_t off = (uint32_t) ch * f->charsize;
    static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

    if (col >= cols || row >= rows)
        return;

    for (uint32_t i = 0; i < FONT_H; i++) {
        for (uint32_t k = 0; k < FONT_W; k++) {
            uint32_t c = (i < f->charsize && (f->data[off + i] & masks[k]))
                ? fg : bg;
            putpixel(col * FONT_W + k, row * FONT_H + i, c);
        }
        mark_row(row * FONT_H + i);
    }
}

static void draw_ch(uint8_t ch)
{
    draw_cell(cx, cy, ch);
}

/* Fill a whole cell with a single colour. */
static void fill_cell(uint32_t col, uint32_t row, uint32_t color)
{
    if (col >= cols || row >= rows)
        return;

    for (uint32_t i = 0; i < FONT_H; i++) {
        for (uint32_t k = 0; k < FONT_W; k++)
            putpixel(col * FONT_W + k, row * FONT_H + i, color);
        mark_row(row * FONT_H + i);
    }
}

/* A block cursor: fill the next cell with the foreground colour, or clear it
 * when hidden. */
static void draw_cursor(bool visible)
{
    fill_cell(cx, cy, visible ? fg : bg);
}

static void scroll(void)
{
    uint64_t line = (uint64_t) FONT_H * fb_pitch;
    uint64_t n = (uint64_t) (fb_h - FONT_H) * fb_pitch;
    uint8_t *d = back;
    uint8_t *s = back + line;

    for (uint64_t i = 0; i < n; i++)
        d[i] = s[i];

    memset(back + n, 0, line);
    mark_all();
}

static void newline(void)
{
    cx = 0;
    if (cy + 1 >= rows)
        scroll();
    else
        cy++;
}

static void putc_raw(uint8_t c)
{
    if (c == '\n') {
        newline();
    } else if (c == '\r') {
        cx = 0;
    } else if (c == '\b') {
        if (cx > 0) {
            cx--;
            draw_ch(' ');
        }
    } else if (c == '\t') {
        uint32_t next = (cx + 8) & ~7u;
        if (next > cols)
            next = cols;
        while (cx < next) {
            draw_ch(' ');
            cx++;
        }
    } else if (c != 0) {
        if (cx >= cols)
            newline();
        draw_ch(c);
        cx++;
    }
}

/* Minimal SGR handling so the kernel's coloured output renders. */
static int esc_state;
static int esc_param;

static void apply_sgr(int p)
{
    switch (p) {
    case 0:
        fg = COLOR_GREY;
        bg = COLOR_BLACK;
        break;
    case 30: fg = COLOR_BLACK; break;
    case 31: fg = COLOR_RED; break;
    case 32: fg = COLOR_GREEN; break;
    case 33: fg = COLOR_YELLOW; break;
    case 34: fg = COLOR_BLUE; break;
    case 35: fg = COLOR_MAGENTA; break;
    case 36: fg = COLOR_CYAN; break;
    case 37: fg = COLOR_GREY; break;
    default: break;
    }
}

static void term_input(uint8_t c)
{
    if (esc_state == 0) {
        if (c == 0x1b) {
            esc_state = 1;
            esc_param = 0;
            return;
        }
        putc_raw(c);
        return;
    }

    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            esc_param = 0;
        } else {
            esc_state = 0;
        }
        return;
    }

    /* Collect a numeric parameter until the final byte. */
    if (c >= '0' && c <= '9') {
        esc_param = esc_param * 10 + (c - '0');
        return;
    }

    if (c == 'm')
        apply_sgr(esc_param);

    esc_state = 0;
}

static void process(sys_ipc_msg_t * m)
{
    if (m->tag != CONSOLE_WRITE_TAG)
        return;

    uint64_t n = m->words[5];
    if (n > 5)
        n = 5;

    for (uint64_t i = 0; i < n; i++)
        term_input((uint8_t) m->words[i]);
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets bootinfo before the task is runnable. */
    }

    fbio = (uint8_t *) bi.fb_vaddr;
    fb_w = bi.fb_width;
    fb_h = bi.fb_height;
    fb_pitch = bi.fb_pitch;
    cols = fb_w / FONT_W;
    rows = fb_h / FONT_H;
    cx = bi.cursor_x;
    cy = bi.cursor_y;
    fg = bi.fgcolor;
    bg = bi.bgcolor;

    back = sys_malloc((uint64_t) fb_pitch * fb_h);
    if (back == NULL)
        return 1;

    /* Keep whatever the kernel already drew. */
    memcpy(back, fbio, (uint64_t) fb_pitch * fb_h);

    /* Blink the cursor while the queue is idle; keep it solid while output
     * is being drawn. */
    bool cursor_drawn = false;
    bool blink_on = true;

    for (;;) {
        sys_ipc_msg_t m;
        int r = sys_ipc_recv_timeout((int64_t) bi.console_ep, &m, 500);

        if (cursor_drawn) {
            draw_cursor(false);
            cursor_drawn = false;
        }

        if (r == 0) {
            process(&m);
            while (sys_ipc_recv_nb((int64_t) bi.console_ep, &m) == 0)
                process(&m);
            blink_on = true;
        } else {
            blink_on = !blink_on;
        }

        if (blink_on) {
            draw_cursor(true);
            cursor_drawn = true;
        }

        flush();
    }

    return 0;
}
