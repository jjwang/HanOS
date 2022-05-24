/**-----------------------------------------------------------------------------

 @file    fb.c
 @brief   Implementation of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer  was got
  from Limine bootloader.

  History:
  Mar 27, 2022 - Rewrite fb_refresh() by memcpy() significantly improve the
                 frame rate. In the future, we should rewrite memcpy() by SSE
                 enhancements.

 @endverbatim
 @todo    Improve memcpy() by SSE enhancements.

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <core/mm.h>
#include <device/display/fb.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/lock.h>

void fb_putch(fb_info_t* fb, uint32_t x, uint32_t y, 
              uint32_t fgcolor, uint32_t bgcolor, uint8_t ch)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    uint32_t offset = ((uint32_t)ch) * 16;
    static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
    for(int i = 0; i < 16; i++){
        for(int k = 0; k < 8; k++){
            if(asc16_font[offset + i] & masks[k]) {
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
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    (void)x;
    (void)y;
    (void)fgcolor;
    (void)bgcolor;
    (void)ch;
#if 0
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
#endif
}

void fb_putpixel(fb_info_t* fb, uint32_t x, uint32_t y, uint32_t color)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x] = color;
    }
}

uint32_t fb_getpixel(fb_info_t* fb, uint32_t x, uint32_t y)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return 0;

    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        return ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x];
    } else {
        return 0;
    }
}

void fb_init(fb_info_t* fb, struct limine_framebuffer* s)
{
    if(s == NULL) {
        if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) {
            fb->backbuffer = kmalloc(fb->backbuffer_len);
            memcpy(fb->backbuffer, fb->addr, fb->backbuffer_len);
        }
        return;
    }
    fb->addr = (uint8_t*)s->address;
    fb->width = s->width;
    fb->height = s->height;
    fb->pitch = s->pitch;

    fb->backbuffer_len = fb->height * fb->pitch;
    fb->backbuffer = fb->addr;

    for(uint32_t x = 0; x < fb->width; x++) {
        for(uint32_t y = 0; y < fb->height; y++) {
            fb_putpixel(fb, x, y, DEFAULT_BGCOLOR);
        }
    }
    fb_refresh(fb);
}

void fb_refresh(fb_info_t* fb)
{
    if((uint64_t)fb->addr != (uint64_t)fb->backbuffer) {
        memcpy(fb->addr, fb->backbuffer, fb->height * fb->pitch * sizeof(uint8_t));
    }
}
