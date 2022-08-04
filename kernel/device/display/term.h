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

void kdisplay(char* s);     /* Implemented in entry C file */

#define TERM_MODE_INFO      0
#define TERM_MODE_CLI       1
#define TERM_MODE_GUI       2
#define TERM_MODE_UNKNOWN   3

typedef struct {
    fb_info_t fb; 

    uint32_t fgcolor;
    uint32_t bgcolor;
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
} term_info_t;

void term_init(struct limine_framebuffer* s);
void term_putch(int mode, uint8_t c);
void term_clear(int mode);
void term_refresh(int mode, bool forced);
void term_start();
void term_switch(int mode);
int term_get_mode(void);
bool term_get_redraw(void);
void term_set_redraw(bool val);
void term_set_cursor(uint8_t c);


