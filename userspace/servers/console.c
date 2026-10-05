/**-----------------------------------------------------------------------------

 @file    console.c
 @brief   Userspace framebuffer console server

 @details
 @verbatim

   The kernel grants this server the framebuffer mapping and an endpoint for
   console bytes. The server feeds the bytes to libvterm, renders the resulting
   VTermScreen cells with the shared gohufont glyphs into a back buffer, and
   blits only the changed rows to the framebuffer.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <string.h>
#include <sysfunc.h>

#include <vterm.h>

/**
 * @brief PSF1 bitmap font header
 */
typedef struct {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
    uint8_t data[];
} psf1_t;

extern psf1_t term_font_norm;

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

#define ROW_MAX     512

static uint8_t *fbio;
static uint8_t *back;
static uint32_t fb_w;
static uint32_t fb_h;
static uint32_t fb_pitch;
static uint32_t cols;
static uint32_t rows;
static uint32_t term_x;         /* top-left pixel of the terminal grid */
static uint32_t term_y;

static VTerm *vt;
static VTermScreen *vs;

static bool row_dirty[ROW_MAX];

/* Cursor state reported by libvterm and the cell currently drawn as a block. */
static int32_t vterm_cur_row;
static int32_t vterm_cur_col;
static bool cursor_visible = true;
static int32_t drawn_row = -1;
static int32_t drawn_col = -1;

/* Rows changed since the last blit, so only those are copied to the
 * framebuffer instead of the whole screen. */
static int32_t dirty_min = -1;
static int32_t dirty_max = -1;

static void mark_row(uint32_t y)
{
    if (dirty_min < 0 || (int32_t) y < dirty_min)
        dirty_min = (int32_t) y;
    if (dirty_max < 0 || (int32_t) y > dirty_max)
        dirty_max = (int32_t) y;
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

/* Draw one glyph at an absolute pixel position. */
static void draw_glyph_px(uint32_t x, uint32_t y, uint8_t ch, uint32_t fgc,
                          uint32_t bgc)
{
    psf1_t *f = &term_font_norm;
    uint32_t off = (uint32_t) ch * f->charsize;
    static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };

    for (uint32_t i = 0; i < FONT_H; i++) {
        for (uint32_t k = 0; k < FONT_W; k++) {
            uint32_t c = (i < f->charsize && (f->data[off + i] & masks[k]))
                ? fgc : bgc;
            putpixel(x + k, y + i, c);
        }
        mark_row(y + i);
    }
}

/* Draw one glyph cell, relative to the terminal grid origin. */
static void draw_glyph(uint32_t col, uint32_t row, uint8_t ch, uint32_t fgc,
                       uint32_t bgc)
{
    if (col >= cols || row >= rows)
        return;
    draw_glyph_px(term_x + col * FONT_W, term_y + row * FONT_H, ch, fgc, bgc);
}

static void fill_cell(uint32_t col, uint32_t row, uint32_t color)
{
    if (col >= cols || row >= rows)
        return;

    for (uint32_t i = 0; i < FONT_H; i++) {
        for (uint32_t k = 0; k < FONT_W; k++)
            putpixel(term_x + col * FONT_W + k, term_y + row * FONT_H + i,
                     color);
        mark_row(term_y + row * FONT_H + i);
    }
}

/* A static Matrix-like rain of green characters behind the terminal. It is
 * drawn once for the framebuffer console; a GPU console will animate the
 * stream. Most columns stay dark so the terminal reads clearly. */
static uint32_t bg_rng = 0x9e3779b9u;

static uint32_t bg_rand(void)
{
    bg_rng = bg_rng * 1103515245u + 12345u;
    return bg_rng >> 8;
}

static void draw_background(void)
{
    static const char set[] =
        "01010101010101011123456789ABCDEF#$%&*<>[]{}()/=+-:.";
    uint32_t bc = fb_w / FONT_W;
    uint32_t br = fb_h / FONT_H;
    uint32_t ns = sizeof(set) - 1;

    for (uint32_t c = 0; c < bc; c++) {
        uint32_t head;
        uint32_t tail;

        /* Leave a third of the columns empty. */
        if (bg_rand() % 3 == 0)
            continue;

        /* A short stream per column: a bright head low on the screen and a
         * tail fading upward. */
        head = br - 1 - (bg_rand() % (br ? br : 1));
        tail = 5 + (bg_rand() % 16);

        for (uint32_t r = 0; r < br; r++) {
            uint32_t d;
            uint32_t lvl;
            uint32_t col;
            uint8_t ch;

            if (r > head)
                continue;
            d = head - r;
            if (d >= tail)
                continue;

            lvl = (tail - d) * 255 / tail;
            if (lvl > 210)
                col = 0xddffdd; /* head */
            else if (lvl > 140)
                col = 0x33ff66;
            else if (lvl > 70)
                col = 0x00aa33;
            else
                col = 0x004411; /* tail */

            ch = (uint8_t) set[bg_rand() % ns];
            draw_glyph_px(c * FONT_W, r * FONT_H, ch, col, COLOR_BLACK);
        }
    }
}

static uint32_t color_of(const VTermColor * c, uint32_t def, bool is_fg)
{
    if ((is_fg && VTERM_COLOR_IS_DEFAULT_FG(c))
        || (!is_fg && VTERM_COLOR_IS_DEFAULT_BG(c)))
        return def;

    VTermColor t = *c;

    vterm_screen_convert_color_to_rgb(vs, &t);
    if (VTERM_COLOR_IS_RGB(&t))
        return ((uint32_t) t.rgb.red << 16) | ((uint32_t) t.rgb.green << 8)
            | (uint32_t) t.rgb.blue;
    return def;
}

static void render_cell(uint32_t row, uint32_t col)
{
    VTermScreenCell cell;
    VTermPos pos;
    uint32_t cp;
    uint8_t ch = ' ';
    uint32_t f;
    uint32_t b;

    pos.row = (int) row;
    pos.col = (int) col;
    if (!vterm_screen_get_cell(vs, pos, &cell))
        return;

    cp = cell.chars[0];
    if (cp >= 0x20 && cp < 0x7f)
        ch = (uint8_t) cp;
    else if (cp >= 0xa0 && cp < 0x100)
        ch = (uint8_t) cp;
    else if (cp != 0)
        ch = '?';

    f = color_of(&cell.fg, COLOR_GREY, true);
    b = color_of(&cell.bg, COLOR_BLACK, false);
    if (cell.attrs.reverse) {
        uint32_t t = f;

        f = b;
        b = t;
    }
    if (cell.attrs.conceal) {
        f = b;
    }
    draw_glyph(col, row, ch, f, b);
}

static void render_dirty(void)
{
    for (uint32_t r = 0; r < rows && r < ROW_MAX; r++) {
        if (!row_dirty[r])
            continue;
        for (uint32_t c = 0; c < cols; c++)
            render_cell(r, c);
        row_dirty[r] = false;
    }
}

/* Repaint the previous cursor cell and draw the block when show is set. */
static void put_cursor(bool show)
{
    if (drawn_row >= 0 && drawn_row < (int32_t) rows)
        render_cell((uint32_t) drawn_row, (uint32_t) drawn_col);
    drawn_row = -1;

    if (show && cursor_visible && vterm_cur_row >= 0
        && vterm_cur_row < (int32_t) rows && vterm_cur_col >= 0
        && vterm_cur_col < (int32_t) cols) {
        fill_cell((uint32_t) vterm_cur_col, (uint32_t) vterm_cur_row,
                  COLOR_GREY);
        drawn_row = vterm_cur_row;
        drawn_col = vterm_cur_col;
    }
}

static int on_damage(VTermRect rect, void *user)
{
    (void) user;
    for (int r = rect.start_row; r < rect.end_row && r < (int) rows
         && r < ROW_MAX; r++)
        if (r >= 0)
            row_dirty[r] = true;
    return 1;
}

static int on_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void) oldpos;
    (void) user;
    vterm_cur_row = pos.row;
    vterm_cur_col = pos.col;
    cursor_visible = visible != 0;
    return 1;
}

static int on_settermprop(VTermProp prop, VTermValue * val, void *user)
{
    (void) user;
    if (prop == VTERM_PROP_CURSORVISIBLE)
        cursor_visible = val->boolean != 0;
    return 1;
}

static int on_bell(void *user)
{
    (void) user;
    return 1;
}

static int on_sb_pushline(int cols, const VTermScreenCell * cells, void *user)
{
    (void) cols;
    (void) cells;
    (void) user;
    return 0;
}

static int on_sb_popline(int cols, VTermScreenCell * cells, void *user)
{
    (void) cols;
    (void) cells;
    (void) user;
    return 0;
}

static void feed_msg(sys_ipc_msg_t * m)
{
    uint64_t n = m->words[5];
    /* The tty sends a bare LF; a real terminal maps NL to CR-NL on output. */
    static uint8_t prev = '\n';
    char buf[12];
    int32_t o = 0;

    if (n > 5)
        n = 5;

    for (uint64_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t) m->words[i];

        if (c == '\n' && prev != '\r')
            buf[o++] = '\r';
        buf[o++] = (char) c;
        prev = c;
    }

    if (o > 0)
        vterm_input_write(vt, buf, (size_t) o);
}

int32_t main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets bootinfo before the process is runnable. */
    }

    fbio = (uint8_t *) bi.fb_vaddr;
    fb_w = bi.fb_width;
    fb_h = bi.fb_height;
    fb_pitch = bi.fb_pitch;

    /* The terminal grid fills the middle four fifths; the margins show the
     * background. */
    cols = (fb_w / FONT_W) * 4 / 5;
    rows = (fb_h / FONT_H) * 4 / 5;
    if (rows > ROW_MAX)
        rows = ROW_MAX;
    term_x = (fb_w - cols * FONT_W) / 2;
    term_y = (fb_h - rows * FONT_H) / 2;

    back = sys_malloc((uint64_t) fb_pitch * fb_h);
    if (back == NULL)
        return 1;

    memset(back, 0, (uint64_t) fb_pitch * fb_h);
    draw_background();
    memcpy(fbio, back, (uint64_t) fb_pitch * fb_h);

    vt = vterm_new((int) rows, (int) cols);
    if (vt == NULL)
        return 1;
    vterm_set_utf8(vt, 1);

    vs = vterm_obtain_screen(vt);
    if (vs == NULL)
        return 1;

    static VTermScreenCallbacks callbacks = {
        .damage = on_damage,
        .movecursor = on_movecursor,
        .settermprop = on_settermprop,
        .bell = on_bell,
        .sb_pushline = on_sb_pushline,
        .sb_popline = on_sb_popline,
    };

    vterm_screen_set_callbacks(vs, &callbacks, NULL);
    vterm_screen_set_damage_merge(vs, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(vs, 1);
    vterm_screen_reset(vs, 1);

    /* Paint the empty terminal over the centre; the margins keep the
     * background. */
    for (uint32_t r = 0; r < rows; r++)
        row_dirty[r] = true;
    render_dirty();
    flush();

    bool blink_on = true;

    for (;;) {
        sys_ipc_msg_t m;
        int32_t r = sys_ipc_recv_timeout((int64_t) bi.console_ep, &m, 500);

        if (r == 0) {
            feed_msg(&m);
            while (sys_ipc_recv_nb((int64_t) bi.console_ep, &m) == 0)
                feed_msg(&m);

            vterm_screen_flush_damage(vs);
            render_dirty();
            blink_on = true;    /* solid while output flows */
            put_cursor(true);
            flush();
        } else {
            blink_on = !blink_on;
            put_cursor(blink_on);
            flush();
        }
    }

    return 0;
}
