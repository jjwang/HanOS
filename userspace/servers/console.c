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
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
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
#define COLOR_WHITE     0xFFFFFF

#define ROW_MAX     512
#define COL_MAX     192
#define STATUS_H        20
#define STATUS_BG       0x00101820
#define STATUS_FG       0x00ddffdd
#define STATUS_LINE     0x0000ff88
#define STATUS_ALPHA    0xD0
#define BG_ALPHA        0xCC    /* 80% opaque */

static uint8_t *fbio;
static uint8_t *back;
static uint32_t fb_w;
static uint32_t fb_h;
static uint32_t fb_pitch;
static uint32_t cols;
static uint32_t rows;
static uint32_t term_x;         /* top-left pixel of the terminal grid */
static uint32_t term_y;
static bool console_desktop;
static uint64_t status_last_sec;

static VTerm *vt;
static VTermScreen *vs;

static bool cell_dirty[ROW_MAX][COL_MAX];

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

/* Animated Matrix-like rain behind the terminal. Each column owns a stream:
 * a bright head, a fading tail, its own speed and length, and an idle delay
 * after the tail leaves the screen. The random idle gaps keep the rain
 * intermittent instead of a uniform waterfall. */
#define RAIN_COL_MAX    512
#define RAIN_TICK_MS    33

typedef struct {
    int32_t head;               /* head row; -1 while waiting */
    int32_t len;                /* tail length in rows */
    int32_t speed;              /* rows per tick */
    int32_t delay;              /* idle ticks before the next stream */
    uint32_t seed;              /* per-stream glyph seed */
} rain_col_t;

static rain_col_t rain[RAIN_COL_MAX];
static uint32_t rain_ncol;
static uint32_t rain_nrow;
static uint32_t bg_rng = 0x9e3779b9u;

static uint32_t bg_rand(void)
{
    bg_rng = bg_rng * 1103515245u + 12345u;
    return bg_rng >> 8;
}

static uint32_t rain_hash(uint32_t seed, uint32_t row)
{
    uint32_t x = seed + row * 0x9e3779b9u;

    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    return x;
}

static void rain_start(rain_col_t * col)
{
    col->len = 4 + (int32_t) (bg_rand() % 14);
    col->speed = 1 + (int32_t) (bg_rand() % 3);
    col->seed = bg_rand() | 1;
    col->head = -(int32_t) (bg_rand() % (uint32_t) (col->len + 1));
    col->delay = 0;
}

/* A rain cell is visible only outside the terminal grid; the centre belongs
 * to the terminal and must not be overwritten. */
static bool rain_visible(uint32_t col, uint32_t row)
{
    int32_t x0 = (int32_t) (col * FONT_W);
    int32_t y0 = (int32_t) (row * FONT_H);
    int32_t tx0 = (int32_t) term_x;
    int32_t ty0 = (int32_t) term_y;
    int32_t tx1 = tx0 + (int32_t) (cols * FONT_W);
    int32_t ty1 = ty0 + (int32_t) (rows * FONT_H);

    if (x0 + FONT_W <= tx0 || x0 >= tx1)
        return true;
    if (y0 + FONT_H <= ty0 || y0 >= ty1)
        return true;
    return false;
}

static uint32_t rain_color(int32_t d, int32_t len)
{
    uint32_t lvl;

    if (d <= 0)
        return 0xddffdd;
    lvl = (uint32_t) (len - d) * 255u / (uint32_t) len;
    if (lvl > 150)
        return 0x55ff88;
    if (lvl > 90)
        return 0x22cc55;
    if (lvl > 40)
        return 0x00aa33;
    return 0x004411;
}

static void rain_draw_col(uint32_t c)
{
    static const char set[] =
        "01010101010101011123456789ABCDEF#$%&*<>[]{}()/=+-:.";
    rain_col_t *col = &rain[c];
    uint32_t ns = sizeof(set) - 1;

    for (uint32_t r = 0; r < rain_nrow; r++) {
        if (!rain_visible(c, r))
            continue;
        if (col->head >= 0 && (int32_t) r <= col->head
            && (int32_t) r > col->head - col->len) {
            int32_t d = col->head - (int32_t) r;
            uint8_t ch = (uint8_t) set[rain_hash(col->seed, r) % ns];

            draw_glyph_px(c * FONT_W, r * FONT_H, ch,
                          rain_color(d, col->len), COLOR_BLACK);
        } else {
            draw_glyph_px(c * FONT_W, r * FONT_H, ' ',
                          COLOR_BLACK, COLOR_BLACK);
        }
    }
}

static void rain_tick(void)
{
    for (uint32_t c = 0; c < rain_ncol; c++) {
        rain_col_t *col = &rain[c];

        if (col->head < 0) {
            if (col->delay > 0) {
                col->delay--;
                continue;
            }
            rain_start(col);
            rain_draw_col(c);
            continue;
        }

        col->head += col->speed;
        if (col->head - col->len >= (int32_t) rain_nrow) {
            rain_draw_col(c);
            col->head = -1;
            col->delay = 15 + (int32_t) (bg_rand() % 120);
            continue;
        }
        rain_draw_col(c);
    }
}

static void rain_init(void)
{
    rain_ncol = fb_w / FONT_W;
    if (rain_ncol > RAIN_COL_MAX)
        rain_ncol = RAIN_COL_MAX;
    rain_nrow = fb_h / FONT_H;

    for (uint32_t c = 0; c < rain_ncol; c++) {
        rain_col_t *col = &rain[c];

        col->seed = bg_rand() | 1;
        col->head = -1;
        col->delay = (int32_t) (bg_rand() % 120);
        if (bg_rand() % 2) {
            rain_start(col);
            if (bg_rand() % 2)
                col->head = (int32_t) (bg_rand() % rain_nrow);
        }
    }
}

/* A light theme: the terminal sits on white, so the palette darkens the
 * colours that a program picked for a dark background. */
static const uint32_t ansi_light[16] = {
    0x000000,                   /* black   */
    0xAA0000,                   /* red     */
    0x008800,                   /* green   */
    0x806000,                   /* yellow  */
    0x0000AA,                   /* blue    */
    0xAA00AA,                   /* magenta */
    0x006666,                   /* cyan    */
    0x333333,                   /* white   */
    0x555555,                   /* bright black   */
    0xCC0000,                   /* bright red     */
    0x008800,                   /* bright green   */
    0x806000,                   /* bright yellow  */
    0x0000EE,                   /* bright blue    */
    0xCC00CC,                   /* bright magenta */
    0x008888,                   /* bright cyan    */
    0x000000,                   /* bright white   */
};

static uint32_t color_of(const VTermColor * c, uint32_t def, bool is_fg)
{
    if ((is_fg && VTERM_COLOR_IS_DEFAULT_FG(c))
        || (!is_fg && VTERM_COLOR_IS_DEFAULT_BG(c)))
        return def;

    if (VTERM_COLOR_IS_INDEXED(c) && c->indexed.idx < 16)
        return ansi_light[c->indexed.idx];

    VTermColor t = *c;

    vterm_screen_convert_color_to_rgb(vs, &t);
    if (VTERM_COLOR_IS_RGB(&t))
        return ((uint32_t) t.rgb.red << 16) | ((uint32_t) t.rgb.green << 8)
            | (uint32_t) t.rgb.blue;
    return def;
}

/* Premultiplied ARGB for the sprite plane's software alpha mode. */
static uint32_t premul(uint32_t rgb, uint32_t a)
{
    uint32_t r = (((rgb >> 16) & 0xff) * a) / 255;
    uint32_t g = (((rgb >> 8) & 0xff) * a) / 255;
    uint32_t b = ((rgb & 0xff) * a) / 255;

    return (a << 24) | (r << 16) | (g << 8) | b;
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

    f = color_of(&cell.fg, COLOR_BLACK, true);
    b = color_of(&cell.bg, COLOR_WHITE, false);
    if (cell.attrs.reverse) {
        uint32_t t = f;

        f = b;
        b = t;
    }
    if (cell.attrs.conceal) {
        f = b;
    }
    if (console_desktop) {
        f = premul(f, 0xFF);
        b = premul(b, BG_ALPHA);
    } else {
        f |= 0xFF000000u;
        b |= 0xFF000000u;
    }
    draw_glyph(col, row, ch, f, b);
}

static void render_dirty(void)
{
    for (uint32_t r = 0; r < rows && r < ROW_MAX; r++)
        for (uint32_t c = 0; c < cols && c < COL_MAX; c++)
            if (cell_dirty[r][c]) {
                render_cell(r, c);
                cell_dirty[r][c] = false;
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
                  0xFF000000);
        drawn_row = vterm_cur_row;
        drawn_col = vterm_cur_col;
    }
}

static int on_damage(VTermRect rect, void *user)
{
    (void) user;
    for (int r = rect.start_row; r < rect.end_row && r < (int) rows
         && r < ROW_MAX; r++) {
        if (r < 0)
            continue;
        for (int c = rect.start_col;
             c < rect.end_col && c < (int) cols && c < COL_MAX; c++)
            if (c >= 0)
                cell_dirty[r][c] = true;
    }
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
    char buf[96];
    int32_t o = 0;

    if (n > 40)
        n = 40;

    for (uint64_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t) (m->words[i / 8] >> ((i % 8) * 8));

        if (c == '\n' && prev != '\r')
            buf[o++] = '\r';
        buf[o++] = (char) c;
        prev = c;
    }

    if (o > 0)
        vterm_input_write(vt, buf, (size_t) o);
}

/* Desktop status bar: version, wall clock and the rain source. */
static void draw_status(void)
{
    struct utsname u;
    struct timespec ts;
    char buf[96];
    uint32_t sec;
    int h;
    int m;
    int s;

    if (uname(&u) != 0)
        u.version[0] = '\0';
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        ts.tv_sec = 0;
    sec = (uint32_t) (ts.tv_sec % 86400);
    h = (int) (sec / 3600);
    m = (int) ((sec / 60) % 60);
    s = (int) (sec % 60);
    snprintf(buf, sizeof(buf), "HanOS %s  %02d:%02d:%02d  rain:gpu",
             u.release, h, m, s);

    uint32_t bg = console_desktop ? premul(STATUS_BG, STATUS_ALPHA)
        : (STATUS_BG | 0xFF000000u);
    uint32_t line = console_desktop ? premul(STATUS_LINE, 0xFF)
        : (STATUS_LINE | 0xFF000000u);
    uint32_t fg = console_desktop ? premul(STATUS_FG, 0xFF)
        : (STATUS_FG | 0xFF000000u);

    for (uint32_t y = 0; y < STATUS_H; y++) {
        for (uint32_t x = 0; x < fb_w; x++)
            putpixel(x, y, (y == STATUS_H - 1) ? line : bg);
        mark_row(y);
    }
    for (uint32_t i = 0; buf[i] != '\0'; i++)
        draw_glyph_px(8 + i * 8, 2, (uint8_t) buf[i], fg, bg);
}

static void status_tick(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0)
        return;
    if ((uint64_t) ts.tv_sec != status_last_sec) {
        status_last_sec = (uint64_t) ts.tv_sec;
        draw_status();
    }
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
    console_desktop = bi.desktop != 0;
    if (console_desktop) {
        cols = fb_w / FONT_W;
        rows = (fb_h - STATUS_H) / FONT_H;
        if (rows > ROW_MAX)
            rows = ROW_MAX;
        term_x = 0;
        term_y = STATUS_H;
    } else {
        cols = (fb_w / FONT_W) * 4 / 5;
        rows = (fb_h / FONT_H) * 4 / 5;
        if (rows > ROW_MAX)
            rows = ROW_MAX;
        term_x = (fb_w - cols * FONT_W) / 2;
        term_y = (fb_h - rows * FONT_H) / 2;
    }

    /* Publish the grid so a program can size and centre its output. */
    struct winsize ws = { (unsigned short) rows, (unsigned short) cols, 0, 0 };

    ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws);

    back = sys_malloc((uint64_t) fb_pitch * fb_h);
    if (back == NULL)
        return 1;

    memset(back, 0, (uint64_t) fb_pitch * fb_h);
    if (console_desktop) {
        draw_status();
    } else {
        rain_init();
        rain_tick();
    }
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
    vterm_screen_set_damage_merge(vs, VTERM_DAMAGE_CELL);
    vterm_screen_enable_altscreen(vs, 1);
    vterm_screen_reset(vs, 1);

    /* Paint the empty terminal over the centre; the margins keep the
     * background. */
    for (uint32_t r = 0; r < rows && r < ROW_MAX; r++)
        for (uint32_t c = 0; c < cols && c < COL_MAX; c++)
            cell_dirty[r][c] = true;
    render_dirty();
    flush();

    bool blink_on = true;
    uint32_t blink_ticks = 0;

    for (;;) {
        sys_ipc_msg_t m;
        int32_t r = sys_ipc_recv_timeout((int64_t) bi.console_ep, &m,
                                         RAIN_TICK_MS);

        if (r == 0) {
            feed_msg(&m);
            while (sys_ipc_recv_nb((int64_t) bi.console_ep, &m) == 0)
                feed_msg(&m);

            vterm_screen_flush_damage(vs);
            render_dirty();
            blink_on = true;    /* solid while output flows */
            blink_ticks = 0;
            put_cursor(true);
            flush();
        } else {
            /* No terminal output for a tick: advance the rain (fallback) or
             * the status clock, and blink the cursor every 500 ms. */
            if (console_desktop)
                status_tick();
            else
                rain_tick();
            blink_ticks++;
            if (blink_ticks >= 500 / RAIN_TICK_MS) {
                blink_ticks = 0;
                blink_on = !blink_on;
                put_cursor(blink_on);
            }
            flush();
        }
    }

    return 0;
}
