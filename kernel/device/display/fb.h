/**-----------------------------------------------------------------------------

 @file    fb.h
 @brief   Definition of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer  was got
  from Limine bootloader.

 @endverbatim
 @author  JW
 @date    Nov 20, 2021

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <3rd-party/boot/stivale2.h>
#include <device/display/font.h>

#define FB_WIDTH            800
#define FB_HEIGHT           600
#define FB_PITCH            (FB_WIDTH * 4)

#define COLOR_BLACK         0x000000
#define COLOR_RED           0xFF7777
#define COLOR_GREEN         0x77FF77
#define COLOR_YELLOW        0xFFFF77
#define COLOR_BLUE          0x7777FF
#define COLOR_WHITE         0xFFFFFF
#define COLOR_LIGHT_WHITE   0xC0C0C0

#define DEFAULT_FGCOLOR     COLOR_LIGHT_WHITE
#define DEFAULT_BGCOLOR     COLOR_BLACK

typedef struct {
    uint8_t* addr;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  *backbuffer;
    uint32_t backbuffer_len;
} fb_info_t;

void fb_init(fb_info_t* fb, struct stivale2_struct_tag_framebuffer* s);
void fb_putpixel(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t color);
void fb_putch(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t fgcolor, uint32_t bgcolor, uint8_t ch);
void fb_putzh(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t fgcolor, uint32_t bgcolor, uint8_t* ch);
uint32_t fb_getpixel(fb_info_t* fb, uint32_t x, uint32_t y);
void fb_refresh(fb_info_t* fb);
