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
#include <device/display/term.h>
#include <lib/klog.h>
#include <3rd-party/boot/stivale2.h>

static const uint32_t font_colors[6] = { 
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, DEFAULT_FGCOLOR,
};

static term_info_t term_info = {0};

static bool term_parse_cmd(uint8_t byte)
{
    if (term_info.state == STATE_UNKNOWN) {
        return false;
    }

    if (byte > 0xA0 && term_info.lastch == 0) {
        term_info.lastch = byte;
        goto succ;
    }

    if (term_info.state == STATE_IDLE) {
        if (byte == 0x3F)
            term_info.state = STATE_CMD;
        else
            goto err;
    } else if (term_info.state == STATE_CMD) {
        if (byte == '[') {
            term_info.cparamcount = 1;
            term_info.cparams[0] = 0;
            term_info.state = STATE_PARAM;
        } else
            goto err;
    } else if (term_info.state == STATE_PARAM) {
        if (byte == ';') {
            term_info.cparams[term_info.cparamcount++] = 0;
        } else if (byte == 'm') {
            if (term_info.cparams[0] == 0) {
                term_info.fgcolor = DEFAULT_FGCOLOR;
                term_info.bgcolor = DEFAULT_BGCOLOR;
            } else if (term_info.cparams[0] >= 10 && term_info.cparams[0] <= 15) {
                term_info.fgcolor = font_colors[term_info.cparams[0] - 10];
            } else if (term_info.cparams[0] >= 20 && term_info.cparams[0] <= 25) {
                term_info.bgcolor = font_colors[term_info.cparams[0] - 20];
            }
            goto succ;
        } else if (byte >= '0' && byte <= '9') {
            term_info.cparams[term_info.cparamcount - 1] *= 10;
            term_info.cparams[term_info.cparamcount - 1] += byte - '0';
        } else {
            goto err;
        }
    } else {
        goto err;
    }
    return true;

err:
    term_info.state = STATE_IDLE;
    term_info.cparamcount = 0;
    return false;

succ:
    term_info.state = STATE_IDLE;
    term_info.cparamcount = 0;
    return true;
}

static void term_scroll()
{
    if (term_info.state == STATE_UNKNOWN) {
        return;
    }

    for (size_t y = 0; y < (term_info.cursor_y - 1) * FONT_HEIGHT; y++)
        for (size_t x = 0; x < term_info.fb.width; x++)
            fb_putpixel(&(term_info.fb), x, y, fb_getpixel(&(term_info.fb), x, y + FONT_HEIGHT));

    for (size_t y = (term_info.cursor_y - 1) * FONT_HEIGHT; y < term_info.fb.height; y++)
        for (size_t x = 0; x < term_info.fb.width; x++)
            fb_putpixel(&(term_info.fb), x, y, term_info.bgcolor);
}

void term_refresh()
{
    if (term_info.state == STATE_UNKNOWN) {
        return;
    }

    fb_refresh(&(term_info.fb));
}

void term_clear()
{
    if (term_info.state == STATE_UNKNOWN) {
        return;
    }

    for (size_t y = 0; y < term_info.fb.height; y++)
        for (size_t x = 0; x < term_info.fb.width; x++)
            fb_putpixel(&(term_info.fb), x, y, term_info.bgcolor);

    term_info.cursor_x = 0;
    term_info.cursor_y = 0;
}

void term_putch(uint8_t c)
{
    if (term_info.state == STATE_UNKNOWN) {
        return;
    }

    if (term_parse_cmd(c))
        return;

    if (term_info.cursor_y == term_info.height && c != '\0') {
        term_scroll();
        term_info.cursor_y--;
    }

    switch (c) {
    case '\0':
        return;
    case '\n':
        term_info.cursor_x = 0;
        term_info.cursor_y++;
        break;
    case '\t':
        term_info.cursor_x += (term_info.cursor_x % 4 == 0) ? 0 : (4 - term_info.cursor_x % 4);
        if (term_info.cursor_x > term_info.width) {
            term_info.cursor_x -= term_info.width;
            term_info.cursor_y++;
        }
        break;
    default:
        if (c <= 0xA0 || (c > 0xA0 && term_info.lastch == 0)) {
            if (term_info.cursor_x >= term_info.width) {
                term_info.cursor_x = 0;
                term_info.cursor_y++;
            }
            fb_putch(&(term_info.fb), term_info.cursor_x * FONT_WIDTH, term_info.cursor_y * FONT_HEIGHT,
                     term_info.fgcolor, term_info.bgcolor, c);
            term_info.cursor_x++;
        } else {
            if (term_info.cursor_x >= term_info.width - 1) {
                term_info.cursor_x = 0;
                term_info.cursor_y++;
            }
            uint8_t c3[3] = {term_info.lastch, c, 0};
            term_info.lastch = 0;
            fb_putzh(&(term_info.fb), term_info.cursor_x * FONT_WIDTH, term_info.cursor_y * FONT_HEIGHT,
                     term_info.fgcolor, term_info.bgcolor, c3);
            term_info.cursor_x += 2;
        }
    }

    while(1) {
        if (term_info.cursor_y >= term_info.height
            && !(term_info.cursor_y == term_info.height && term_info.cursor_x == 0)) {
            term_scroll();
            term_info.cursor_y--;
        } else {
            break;
        }
    }
}

void term_init(struct stivale2_struct_tag_framebuffer* s)
{
    fb_init(&(term_info.fb), s);

    term_info.width = term_info.fb.width / FONT_WIDTH;
    term_info.height = term_info.fb.height / FONT_HEIGHT;

    term_info.fgcolor = DEFAULT_FGCOLOR;
    term_info.bgcolor = DEFAULT_BGCOLOR;

    term_info.state = STATE_IDLE;

    term_info.cursor_x = 0;
    term_info.cursor_y = 0;
    term_info.lastch = 0;

    term_clear();
    term_refresh();

    klog_printf("Terminal width: %d, height: %d, pitch: %d, addr: %x\n", 
                term_info.fb.width, term_info.fb.height, term_info.fb.pitch,
                term_info.fb.addr);
}

void term_start()
{
    fb_init(&(term_info.fb), NULL);
}
