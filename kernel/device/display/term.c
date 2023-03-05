/**-----------------------------------------------------------------------------

 @file    term.c
 @brief   Implementation of framebuffer terminal related functions
 @details
 @verbatim

  A framebuffer based terminal was implemented. As the first step, it
  mainly supports information display.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <kconfig.h>
#include <device/display/term.h>
#include <lib/klog.h>
#include <lib/lock.h>
#include <lib/memutils.h>
#include <sys/panic.h>
#include <3rd-party/boot/limine.h>

#if LAUNCHER_GRAPHICS
static const uint32_t font_colors[9] = { 
    COLOR_BLACK,
    COLOR_RED,
    COLOR_GREEN,
    COLOR_BROWN,
    COLOR_BLUE,
    COLOR_MAGENTA,
    COLOR_CYAN,
    COLOR_GREY,
    DEFAULT_FGCOLOR,
};

static term_info_t term_info = {0};
static term_info_t term_cli = {0};
static int term_active_mode = TERM_MODE_UNKNOWN; 
static uint8_t term_cursor = 0;
static lock_t term_lock = {0};
static bool term_need_redraw = false;
#endif

term_cursor_visible_t cursor_visible = CURSOR_INVISIBLE;

#define CHECK_ACTIVE_TERM()     { if (term_act == NULL) kpanic("Active terminal does not exist"); }

void term_get_winsize(winsize_t *ws)
{
    if (ws == NULL) return;

    if (term_cli.state == STATE_IDLE) {
        ws->col = term_cli.width;
        ws->row = term_cli.height;
        ws->xpixel = term_cli.fb.width;
        ws->ypixel = term_cli.fb.height;
    }
}

bool term_set_winsize(winsize_t *ws)
{
    if (ws == NULL) return false;

    if (term_cli.state == STATE_IDLE) {
        if (ws->col != term_cli.width || ws->row != term_cli.height
            || ws->xpixel != term_cli.fb.width
            || ws->ypixel != term_cli.fb.height)
        {
            kpanic("Can't support specified terminal window size");
        }
    }

    return false; 
}

bool term_set_bg_image(image_t *img)
{
    (void)img;
#if LAUNCHER_GRAPHICS
    return fb_set_bg_image(&(term_cli.fb), img);
#else
    return false;
#endif
}

bool term_parse_cmd(term_info_t* term_act, uint8_t byte)
{
    (void)term_act;
    (void)byte;
#if LAUNCHER_GRAPHICS
    CHECK_ACTIVE_TERM();

    if (term_act->state == STATE_UNKNOWN) {
        return false;
    }

    if (byte > 0xA0 && term_act->lastch == 0) {
        term_act->lastch = byte;
        goto succ;
    }

    if (term_act->state == STATE_IDLE) {
        char *s = "\e";
        if (byte == s[0]) {
            if (!term_act->last_qu_char) {
                term_act->state = STATE_CMD;
                term_act->last_qu_char = true;
            } else {
                term_act->last_qu_char = false;
                goto err;
            }
        }
        else
            goto err;
    } else if (term_act->state == STATE_CMD) {
        if (byte == '[') {
            term_act->cparamcount = 1;
            term_act->cparams[0] = 0;
            term_act->state = STATE_PARAM;
        } else
            goto err;
    } else if (term_act->state == STATE_PARAM) {
        if (byte == ';') {
            term_act->cparams[term_act->cparamcount++] = 0;
        } else if (byte == 'm') {
            if (term_act->cparams[0] == 0) {
                term_act->fgcolor = DEFAULT_FGCOLOR;
                term_act->bgcolor = DEFAULT_BGCOLOR;
            } else if (term_act->cparams[0] >= 30 && term_act->cparams[0] <= 37) {
                term_act->fgcolor = font_colors[term_act->cparams[0] - 30];
            } else if (term_act->cparams[0] >= 40 && term_act->cparams[0] <= 47) {
                term_act->bgcolor = font_colors[term_act->cparams[0] - 40];
            }
            goto succ;
        } else if (byte >= '0' && byte <= '9') {
            term_act->cparams[term_act->cparamcount - 1] *= 10;
            term_act->cparams[term_act->cparamcount - 1] += byte - '0';
        } else {
            goto err;
        }
    } else {
        goto err;
    }
    return true;

err:
    term_act->state = STATE_IDLE;
    term_act->cparamcount = 0;
    return false;

succ:
    term_act->state = STATE_IDLE;
    term_act->cparamcount = 0;
    return true;
#else
    return false;
#endif
}

void term_scroll(term_info_t* term_act)
{
    (void)term_act;
#if LAUNCHER_GRAPHICS
    CHECK_ACTIVE_TERM();

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }

    for (size_t y = 0; y < (term_act->cursor_y - 1) * FONT_HEIGHT; y++) {
        for (size_t x = 0; x < term_act->fb.width; x++) {
            uint32_t c = fb_getpixel(&(term_act->fb), x, y + FONT_HEIGHT);
            fb_putpixel(&(term_act->fb), x, y, c);
        }
    }

    for (size_t y = (term_act->cursor_y - 1) * FONT_HEIGHT;
         y < term_act->fb.height;
         y++)
    {
        for (size_t x = 0; x < term_act->fb.width; x++) {
            fb_putpixel(&(term_act->fb), x, y, term_act->bgcolor);
        }
    }
#endif
}

void term_set_cursor(uint8_t c)
{
    (void)c;
#if LAUNCHER_GRAPHICS
    term_cursor = c;
#endif
}

int term_get_mode(void)
{
#if LAUNCHER_GRAPHICS
    return term_active_mode;
#else
    return TERM_MODE_UNKNOWN;
#endif
}

void term_refresh(int mode)
{
    (void)mode;
#if LAUNCHER_GRAPHICS
    term_info_t* term_act;

    lock_lock(&term_lock);

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
        if( term_cursor != 0) {
            if (term_act->state != STATE_UNKNOWN && mode == term_active_mode) {
                fb_refresh(&(term_act->fb));
            }

            uint32_t x = term_act->cursor_x, y = term_act->cursor_y;
            if (x >= term_act->width) {
                x = 0;
                y++;
            }
 
            if (y >= term_act->height) {
                term_scroll(term_act);
                y--;
                term_act->cursor_y--;
            }

            fb_putch(&(term_act->fb), x * FONT_WIDTH, y * FONT_HEIGHT,
                     term_act->fgcolor, term_act->bgcolor, term_cursor);
            if (mode == term_active_mode) {
                fb_refresh(&(term_act->fb));
            }
            lock_release(&term_lock);
            return;
        }
    }

    if (term_act->state == STATE_UNKNOWN) {
        lock_release(&term_lock);
        return;
    }

    if (mode == term_active_mode) {
        fb_refresh(&(term_act->fb));
    }

    lock_release(&term_lock);
#endif
}

void term_clear(int mode)
{
    (void)mode;
#if LAUNCHER_GRAPHICS
    term_info_t* term_act;

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
    }

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }

    if (term_act->fb.bgbuffer != NULL) {
        memcpy(term_act->fb.addr, term_act->fb.bgbuffer,
               term_act->fb.width * term_act->fb.height * 4);
    }

    for (size_t y = 0; y < term_act->fb.height; y++)
        for (size_t x = 0; x < term_act->fb.width; x++)
            fb_putpixel(&(term_act->fb), x, y, term_act->bgcolor);

    term_act->cursor_x = 0;
    term_act->cursor_y = 0;
#endif
}

void term_print(int mode, uint8_t c)
{
    (void)mode;
    (void)c;
#if LAUNCHER_GRAPHICS
    term_info_t* term_act;

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
    }

    if (c == '\b') {
        term_print(mode, ' ');
        if (term_act->cursor_x > 0) term_act->cursor_x--;
        if (term_act->cursor_x > 0) term_act->cursor_x--;
        return;
    }

    if (term_act->cursor_y == term_act->height && c != '\0') {
        term_scroll(term_act);
        term_act->cursor_y--;
    }

    switch (c) {
    case '\0':
        return;
    case '\n':
        term_act->cursor_x = 0;
        term_act->cursor_y++;
        break;
    case '\t':
        term_act->cursor_x += (term_act->cursor_x % 4 == 0) ? 0 : (4 - term_act->cursor_x % 4);
        if (term_act->cursor_x > term_act->width) {
            term_act->cursor_x -= term_act->width;
            term_act->cursor_y++;
        }
        break;
    default:
        if (c <= 0xA0 || (c > 0xA0 && term_act->lastch == 0)) {
            if (term_act->cursor_x >= term_act->width) {
                term_act->cursor_x = 0;
                term_act->cursor_y++;
            }
            if (term_act->cursor_y >= term_act->height) {
                term_scroll(term_act);
                term_act->cursor_y--;
            }
            fb_putch(&(term_act->fb), term_act->cursor_x * FONT_WIDTH, term_act->cursor_y * FONT_HEIGHT,
                     term_act->fgcolor, term_act->bgcolor, c);
            term_act->cursor_x++;
        } else {
            if (term_act->cursor_x >= term_act->width - 1) {
                term_act->cursor_x = 0;
                term_act->cursor_y++;
            }
            if (term_act->cursor_y >= term_act->height) {
                term_scroll(term_act);
                term_act->cursor_y--;
            }

            /* Do not display Chinese character */
            /* uint8_t c3[3] = {term_act->lastch, c, 0}; */

            term_act->lastch = 0;
            fb_putch(&(term_act->fb), term_act->cursor_x * FONT_WIDTH, term_act->cursor_y * FONT_HEIGHT,
                     term_act->fgcolor, term_act->bgcolor, '?');
            fb_putch(&(term_act->fb), (term_act->cursor_x + 1) * FONT_WIDTH, term_act->cursor_y * FONT_HEIGHT,
                     term_act->fgcolor, term_act->bgcolor, '?');
            term_act->cursor_x += 2;
        }
    }

    while(1) {
        if (term_act->cursor_y >= term_act->height
            && !(term_act->cursor_y == term_act->height && term_act->cursor_x == 0)) {
            term_scroll(term_act);
            term_act->cursor_y--;
        } else {
            break;
        }
    }
#endif
}

void term_putch(int mode, uint8_t c)
{
    (void)mode;
    (void)c;
#if LAUNCHER_GRAPHICS
    term_info_t* term_act;

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
    }

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }   

    if (term_act->last_qu_char && c != '[') {
        term_act->state = STATE_IDLE;

        /* Resend the '\e' character */
        char *s = "\e";
        term_print(mode, s[0]);
        term_act->last_qu_char = false;

        term_putch(mode, c);
        return;
    } else {
        term_act->last_qu_char = false;
        if (term_parse_cmd(term_act, c))
            return;
    }   

    term_print(mode, c);
#endif  
}

void term_init(struct limine_framebuffer* s)
{
    (void)s;
#if LAUNCHER_GRAPHICS
    term_info_t* term_act;
    
    term_lock = lock_new();

    for (size_t i = 0; i <= 1; i++) {
        term_act = ((i == 0) ? &term_info : &term_cli);

        fb_init(&(term_act->fb), s);
        term_act->width = term_act->fb.width / FONT_WIDTH;
        term_act->height = term_act->fb.height / FONT_HEIGHT;

        term_act->fgcolor = DEFAULT_FGCOLOR;
        term_act->bgcolor = DEFAULT_BGCOLOR;

        term_act->state = STATE_IDLE;

        term_act->cursor_x = 0;
        term_act->cursor_y = 0;
        term_act->lastch = 0;

        term_clear((i == 0) ? TERM_MODE_INFO : TERM_MODE_CLI);
        term_refresh((i == 0) ? TERM_MODE_INFO : TERM_MODE_CLI);

        klogi("Terminal %d (0x%x) width: %d, height: %d, pitch: %d, addr: %x\n", 
                i, (uint64_t)term_act, term_act->fb.width,
                term_act->fb.height, term_act->fb.pitch, term_act->fb.addr);
    }
#endif
}

void term_start()
{
#if LAUNCHER_GRAPHICS
    fb_init(&(term_info.fb), NULL);
    fb_init(&(term_cli.fb), NULL);

    klog_refresh(TERM_MODE_INFO);
    klog_refresh(TERM_MODE_CLI);

#if LAUNCHER_CLI
    term_active_mode = TERM_MODE_CLI;

    term_clear(term_active_mode);
    fb_putlogo(&(term_cli.fb), COLOR_CYAN, DEFAULT_BGCOLOR);
    term_refresh(TERM_MODE_CLI);
#else
    term_active_mode = TERM_MODE_INFO;
#endif

    term_need_redraw = true;
#endif
}

bool term_get_redraw()
{
#if LAUNCHER_GRAPHICS
    return term_need_redraw;
#else
    return false;
#endif
}

void term_set_redraw(bool val)
{
    (void)val;
#if LAUNCHER_GRAPHICS
    term_need_redraw = val;
#endif
}

void term_switch(int mode)
{
    (void)mode;
#if LAUNCHER_GRAPHICS
    term_active_mode = mode;
#endif
}
