///-----------------------------------------------------------------------------
///
/// @file    term.h
/// @brief   Definition of framebuffer terminal related functions
/// @details
///   A framebuffer based terminal was implemented. As the first step, it
///   mainly supports information display.
/// @author  JW
/// @date    Nov 20, 2021
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <device/fb.h>

#define FONT_WIDTH          8
#define FONT_HEIGHT         16

typedef struct {
    fb_info_t fb; 

    uint32_t fgcolor;
    uint32_t bgcolor;
    uint32_t width;
    uint32_t height;
    uint32_t cursor_x;
    uint32_t cursor_y;

    enum {
        STATE_IDLE,
        STATE_CMD,
        STATE_PARAM
    } state;
    int cparams[16];
    int cparamcount;
    uint8_t lastch;
} term_info_t;

void term_init(term_info_t* t, struct stivale2_struct_tag_framebuffer* s);
void term_putch(term_info_t* t, uint8_t ch);
void term_clear(term_info_t* t);
void term_refresh(term_info_t* t);
