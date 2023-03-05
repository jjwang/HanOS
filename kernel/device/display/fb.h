/**-----------------------------------------------------------------------------

 @file    fb.h
 @brief   Definition of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer  was got
  from Limine bootloader.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <3rd-party/boot/limine.h>
#include <device/display/font.h>
#include <lib/image.h>

#define FB_WIDTH            2048
#define FB_HEIGHT           1536
#define FB_PITCH            (FB_WIDTH * 4)

#define COLOR_BLACK         0x000000
#define COLOR_RED           0xAA0000
#define COLOR_GREEN         0x00AA00
#define COLOR_YELLOW        0xAAAA00
#define COLOR_BROWN         0xAA5500
#define COLOR_BLUE          0x0000AA
#define COLOR_MAGENTA       0xAA00AA
#define COLOR_CYAN          0x00AAAA
#define COLOR_GREY          0xAAAAAA

#define DEFAULT_FGCOLOR     COLOR_GREY
#define DEFAULT_BGCOLOR     COLOR_BLACK

typedef struct {
    uint8_t  *addr;

    uint8_t  *bgbuffer;
    uint8_t  *swapbuffer;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  *backbuffer;
    uint32_t backbuffer_len;;

    image_t  img_bg;
} fb_info_t;

void fb_init(fb_info_t* fb, struct limine_framebuffer* s);
void fb_putpixel(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t color);
void fb_putch(fb_info_t* fb, uint32_t x, uint32_t y,
              uint32_t fgcolor, uint32_t bgcolor, uint8_t ch);
void fb_putlogo(fb_info_t* fb, uint32_t fgcolor, uint32_t bgcolor);
uint32_t fb_getpixel(fb_info_t* fb, uint32_t x, uint32_t y);
void fb_refresh(fb_info_t* fb);
bool fb_set_bg_image(fb_info_t *fb, image_t *img);
