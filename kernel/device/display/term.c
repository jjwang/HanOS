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
#include <device/display/term.h>
#include <lib/klog.h>
#include <core/panic.h>
#include <3rd-party/boot/stivale2.h>

static const uint32_t font_colors[6] = { 
    COLOR_BLACK, COLOR_RED, COLOR_GREEN, COLOR_YELLOW, COLOR_BLUE, DEFAULT_FGCOLOR,
};

static term_info_t term_info = {0};
static term_info_t term_cli = {0};
static int term_active_mode = TERM_MODE_UNKNOWN; 
static uint8_t  term_cursor = 0;

#define FONT_WIDTH          8
#define FONT_HEIGHT         16

#define CHECK_ACTIVE_TERM()     { if (term_act == NULL) kpanic("Active terminal does not exist"); }

static bool term_parse_cmd(term_info_t* term_act, uint8_t byte)
{
    CHECK_ACTIVE_TERM();

    if (term_act->state == STATE_UNKNOWN) {
        return false;
    }

    if (byte > 0xA0 && term_act->lastch == 0) {
        term_act->lastch = byte;
        goto succ;
    }

    if (term_act->state == STATE_IDLE) {
        if (byte == '?' /*0x3F*/) {
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
            } else if (term_act->cparams[0] >= 10 && term_act->cparams[0] <= 15) {
                term_act->fgcolor = font_colors[term_act->cparams[0] - 10];
            } else if (term_act->cparams[0] >= 20 && term_act->cparams[0] <= 25) {
                term_act->bgcolor = font_colors[term_act->cparams[0] - 20];
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
}

static void term_scroll(term_info_t* term_act)
{
    CHECK_ACTIVE_TERM();

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }

    for (size_t y = 0; y < (term_act->cursor_y - 1) * FONT_HEIGHT; y++)
        for (size_t x = 0; x < term_act->fb.width; x++)
            fb_putpixel(&(term_act->fb), x, y, fb_getpixel(&(term_act->fb), x, y + FONT_HEIGHT));

    for (size_t y = (term_act->cursor_y - 1) * FONT_HEIGHT; y < term_act->fb.height; y++)
        for (size_t x = 0; x < term_act->fb.width; x++)
            fb_putpixel(&(term_act->fb), x, y, term_act->bgcolor);
}

void term_set_cursor(uint8_t c)
{
    term_cursor = c;
}

int term_get_mode(void)
{
    return term_active_mode;
}

void term_refresh(int mode)
{
    term_info_t* term_act;

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
        if( term_cursor != 0) {
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
        }
    }

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }

    if (mode == term_active_mode) {
        fb_refresh(&(term_act->fb));
    }
}

void term_clear(int mode)
{
    term_info_t* term_act;

    if (mode == TERM_MODE_INFO) {
        term_act = &term_info;
    } else {
        term_act = &term_cli;
    }

    if (term_act->state == STATE_UNKNOWN) {
        return;
    }

    for (size_t y = 0; y < term_act->fb.height; y++)
        for (size_t x = 0; x < term_act->fb.width; x++)
            fb_putpixel(&(term_act->fb), x, y, term_act->bgcolor);

    term_act->cursor_x = 0;
    term_act->cursor_y = 0;
}

void term_print(int mode, uint8_t c)
{
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
            uint8_t c3[3] = {term_act->lastch, c, 0};
            term_act->lastch = 0;
            fb_putzh(&(term_act->fb), term_act->cursor_x * FONT_WIDTH, term_act->cursor_y * FONT_HEIGHT,
                     term_act->fgcolor, term_act->bgcolor, c3);
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
}

void term_putch(int mode, uint8_t c)
{
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

        /* Resend the '?' character */
        term_print(mode, '?');
        term_act->last_qu_char = false;

        term_putch(mode, c);
        return;
    } else {
        term_act->last_qu_char = false;
        if (term_parse_cmd(term_act, c))
            return;
    }   

    term_print(mode, c);  
}
void term_init(struct stivale2_struct_tag_framebuffer* s)
{
    term_info_t* term_act;
    int i;

    for (i = 0; i <= 1; i++) {
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

        klogi("Terminal %d width: %d, height: %d, pitch: %d, addr: %x\n", 
                i, term_act->fb.width, term_act->fb.height, term_act->fb.pitch,
                term_act->fb.addr);
    }
}

void term_start()
{
    fb_init(&(term_info.fb), NULL);
    fb_init(&(term_cli.fb), NULL);

    klog_refresh(TERM_MODE_INFO);
    klog_refresh(TERM_MODE_CLI);
    term_active_mode = TERM_MODE_INFO;
}

void term_switch(int mode)
{
    term_active_mode = mode;
}
