///-----------------------------------------------------------------------------
///
/// @file    fb.c
/// @brief   Implementation of framebuffer related functions
/// @details
///
///   Graphics can be displayed in a linear framebuffer - a simple array mapped
///   in memory that represents the screen. The address of framebuffer  was got
///   from Limine bootloader.
///
/// @author  JW
/// @date    Nov 20, 2021
///
///-----------------------------------------------------------------------------

#include <stddef.h>
#include <core/mm.h>
#include <device/fb.h>

void fb_putch(fb_info_t* fb, uint32_t x, uint32_t y, 
              uint32_t fgcolor, uint32_t bgcolor, uint8_t ch)
{
    uint32_t offset = ((uint32_t)ch) * 16;
    for(int i = 0; i < 16; i++){
        for(int k = 0; k < 8; k++){
            if(asc16_font[offset + i] & (0x80 >> k)) {
                fb_putpixel(fb, x + k, y + i, fgcolor);
            } else {
                fb_putpixel(fb, x + k, y + i, bgcolor);
            }
        }
    }
}

void fb_putzh(fb_info_t* fb, uint32_t x, uint32_t y,  
              uint32_t fgcolor, uint32_t bgcolor, uint8_t* ch) 
{
    int qh = ch[0] - 0xa1;
    int wh = ch[1] - 0xa1;
    uint32_t offset = (94 * qh + wh) * 16 * 16 / 8;
    for(int i = 0; i < 16; i++){
        for(int n = 0; n < 2; n++) {
            for(int k = 0; k < 8; k++){
                if(hzk16_font[offset + i * 2 + n] & (0x80 >> k)) {
                    fb_putpixel(fb, x + k + n * 8, y + i, fgcolor);
                } else {
                    fb_putpixel(fb, x + k + n * 8, y + i, bgcolor);
                }
            }   
        }   
    }   
}

void fb_putpixel(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t color)
{
    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x] = color;
    }
}

uint32_t fb_getpixel(fb_info_t* fb, uint32_t x, uint32_t y)
{
    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        return ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x];
    } else {
        return 0;
    }
}

void fb_init(fb_info_t* fb, struct stivale2_struct_tag_framebuffer* s)
{
    fb->addr = (uint8_t*)PHYS_TO_VIRT(s->framebuffer_addr);
    fb->width = s->framebuffer_width;
    fb->height = s->framebuffer_height;
    fb->pitch = s->framebuffer_pitch;

    fb->backbuffer_len = fb->height * fb->pitch;
    if(fb->backbuffer_len > sizeof(fb->backbuffer)) {
        fb->backbuffer_len = sizeof(fb->backbuffer);
    }

    for(uint32_t x = 0; x < fb->width; x++) {
        for(uint32_t y = 0; y < fb->height; y++) {
            fb_putpixel(fb, x, y, DEFAULT_BGCOLOR);
        }
    }
    fb_refresh(fb);
}

void fb_refresh(fb_info_t* fb)
{
    for(uint32_t i = 0; i < fb->height * fb->pitch; i++) {
        ((uint8_t*)(fb->addr))[i] = ((uint8_t*)fb->backbuffer)[i];
    }
}
