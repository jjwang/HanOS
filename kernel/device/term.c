///-----------------------------------------------------------------------------
///
/// @file    term.c
/// @brief   Implementation of framebuffer terminal related functions
/// @details
///   A framebuffer based terminal was implemented. As the first step, it
///   mainly supports information display.
/// @author  JW
/// @date    Nov 20, 2021
///
///-----------------------------------------------------------------------------
#include <stddef.h>
#include <device/term.h>
#include <3rd-party/boot/stivale2.h>

static const uint32_t font_colors[6] = { 
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, COLOR_WHITE,
};

static bool term_parse_cmd(term_info_t* t, uint8_t byte)
{
    if (byte > 0xA0 && t->lastch == 0) {
        t->lastch = byte;
        goto succ;
    }

    if (t->state == STATE_IDLE) {
        if (byte == 0x3F)
            t->state = STATE_CMD;
        else
            goto err;
    } else if (t->state == STATE_CMD) {
        if (byte == '[') {
            t->cparamcount = 1;
            t->cparams[0] = 0;
            t->state = STATE_PARAM;
        } else
            goto err;
    } else if (t->state == STATE_PARAM) {
        if (byte == ';') {
            t->cparams[t->cparamcount++] = 0;
        } else if (byte == 'm') {
            if (t->cparams[0] == 0) {
                t->fgcolor = DEFAULT_FGCOLOR;
                t->bgcolor = DEFAULT_BGCOLOR;
            } else if (t->cparams[0] >= 10 && t->cparams[0] <= 15) {
                t->fgcolor = font_colors[t->cparams[0] - 10];
            } else if (t->cparams[0] >= 20 && t->cparams[0] <= 25) {
                t->bgcolor = font_colors[t->cparams[0] - 20];
            }
            goto succ;
        } else if (byte >= '0' && byte <= '9') {
            t->cparams[t->cparamcount - 1] *= 10;
            t->cparams[t->cparamcount - 1] += byte - '0';
        } else {
            goto err;
        }
    } else {
        goto err;
    }
    return true;

err:
    t->state = STATE_IDLE;
    t->cparamcount = 0;
    return false;

succ:
    t->state = STATE_IDLE;
    t->cparamcount = 0;
    return true;
}

static void term_scroll(term_info_t* t)
{
    for (size_t y = 0; y < t->fb.height - FONT_HEIGHT; y++)
        for (size_t x = 0; x < t->fb.width; x++)
            fb_putpixel(&(t->fb), x, y, fb_getpixel(&(t->fb), x, y + FONT_HEIGHT));

    for (size_t y = t->fb.height - FONT_HEIGHT; y < t->fb.height; y++)
        for (size_t x = 0; x < t->fb.width; x++)
            fb_putpixel(&(t->fb), x, y, t->bgcolor);
}

void term_refresh(term_info_t* t)
{
    fb_refresh(&(t->fb));
}

void term_clear(term_info_t* t)
{
    for (size_t y = 0; y < t->fb.height; y++)
        for (size_t x = 0; x < t->fb.width; x++)
            fb_putpixel(&(t->fb), x, y, t->bgcolor);

    t->cursor_x = 0;
    t->cursor_y = 0;
}

void term_putch(term_info_t* t, uint8_t c)
{
    if (term_parse_cmd(t, c))
        return;

    switch (c) {
    case '\0':
        return;
    case '\n':
        t->cursor_x = 0;
        t->cursor_y++;
        break;
    case '\t':
        t->cursor_x += (t->cursor_x % 4 == 0) ? 0 : (4 - t->cursor_x % 4);
        if (t->cursor_x > t->width) {
            t->cursor_x -= t->width;
            t->cursor_y++;
        }
        break;
    default:
        if (c <= 0xA0 || (c > 0xA0 && t->lastch == 0)) {
            if (t->cursor_x >= t->width) {
                t->cursor_x = 0;
                t->cursor_y++;
            }
            fb_putch(&(t->fb), t->cursor_x * FONT_WIDTH, t->cursor_y * FONT_HEIGHT,
                     t->fgcolor, t->bgcolor, c);
            t->cursor_x++;
        } else {
            if (t->cursor_x >= t->width - 1) {
                t->cursor_x = 0;
                t->cursor_y++;
            }
            uint8_t c3[3] = {t->lastch, c, 0};
            t->lastch = 0;
            fb_putzh(&(t->fb), t->cursor_x * FONT_WIDTH, t->cursor_y * FONT_HEIGHT,
                     t->fgcolor, t->bgcolor, c3);
            t->cursor_x += 2;
        }
    }

    while(1) {
        if (t->cursor_y >= t->height) {
            term_scroll(t);
            t->cursor_y--;
        } else {
            break;
        }
    }
}

void term_init(term_info_t* t, struct stivale2_struct_tag_framebuffer* s)
{
    fb_init(&(t->fb), s);

    t->width = t->fb.width / FONT_WIDTH;
    t->height = t->fb.height / FONT_HEIGHT;

    t->fgcolor = DEFAULT_FGCOLOR;
    t->bgcolor = DEFAULT_BGCOLOR;

    t->state = STATE_IDLE;

    t->cursor_x = 0;
    t->cursor_y = 0;
    t->lastch = 0;

    term_clear(t);
    term_refresh(t);
}

