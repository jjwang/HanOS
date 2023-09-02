/**-----------------------------------------------------------------------------

 @file    term.h
 @brief   Definition of framebuffer terminal related functions
 @details
 @verbatim

  A framebuffer based terminal was implemented. As the first step, it
  mainly supports information display.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <device/display/fb.h>
#include <base/image.h>

void kdisplay(char* s);     /* Implemented in entry C file */

#define TERM_MODE_INFO      0
#define TERM_MODE_CLI       1
#define TERM_MODE_GUI       2
#define TERM_MODE_UNKNOWN   3

#define FONT_WIDTH          8
#define FONT_HEIGHT         15

typedef enum {
    CURSOR_INVISIBLE = 0,
    CURSOR_VISIBLE,
    CURSOR_HIDE
} term_cursor_visible_t;

typedef struct {
    fb_info_t fb; 

    uint32_t fgcolor;
    uint32_t bgcolor;
    bool     bold;
    uint32_t width;
    uint32_t height;
    uint32_t cursor_x;
    uint32_t cursor_y;

    enum {
        STATE_UNKNOWN = 0,
        STATE_IDLE,
        STATE_CMD,
        STATE_PARAM
    } state;
    int cparams[16];
    int cparamcount;
    uint8_t lastch;
    bool last_qu_char;
    int skip_left;
} term_info_t;

typedef struct [[gnu::packed]] {
    uint16_t row;           /* rows, in character */
    uint16_t col;           /* columns, in characters */
    uint16_t xpixel;        /* horizontal size, pixels (unused) */
    uint16_t ypixel;        /* vertical size, pixels (unused) */
} winsize_t;

extern term_cursor_visible_t cursor_visible;

void term_init(struct limine_framebuffer* s);
void term_putch(int mode, uint8_t c);
void term_clear(int mode);
void term_refresh(int mode);
void term_start();
void term_switch(int mode);
int term_get_mode(void);
bool term_get_redraw(void);
void term_set_redraw(bool val);
void term_set_cursor(uint8_t c);
bool term_set_bg_image(image_t *img);
void term_get_winsize(winsize_t *ws);
bool term_set_winsize(winsize_t *ws);
