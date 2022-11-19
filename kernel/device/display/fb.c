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
#include <version.h>
#include <core/mm.h>
#include <core/hpet.h>
#include <device/display/fb.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/lock.h>
#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/string.h>

#define LOGO_SCALE      6

static uint64_t fb_refresh_times = 0, fb_refresh_nanos = 0;

bool fb_set_bg_image(fb_info_t *fb, image_t *img)
{
    if (img->bpp == 24 && img->img_width == fb->width
        && img->img_height == fb->height) {
        memcpy(&(fb->img_bg), img, sizeof(image_t));
        if (fb->bg == NULL) {
            fb->bg = (uint8_t*)kmalloc(fb->width * fb->height * 4);
        }

        for (size_t y = 0; y < fb->height; y++) {
            size_t off = img->pitch * (fb->height - 1 - y);
            for (size_t x = 0; x < fb->width; x++) {
                uint8_t *img_pixel = (uint8_t*)img->img + off + x * img->bpp / 8;
                uint8_t r, g, b;
                uint8_t shift = 2;

                b  = img_pixel[0] >> shift;
                g = img_pixel[1] >> shift;
                r = img_pixel[2] >> shift;

                uint32_t color;
                color = (uint32_t)b + ((uint32_t)g << 8) + ((uint32_t)r << 16);
                ((uint32_t*)(fb->bg + (fb->pitch * y)))[x] = color;
            }
        }
        memcpy(fb->backbuffer, fb->bg, fb->width * fb->height * 4); 
        return true;
    }   
    return false;
}

void fb_putch(fb_info_t *fb, uint32_t x, uint32_t y, 
              uint32_t fgcolor, uint32_t bgcolor, uint8_t ch)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    uint32_t offset = ((uint32_t)ch) * 16;
    static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
    for(size_t i = 0; i < 16; i++){
        for(size_t k = 0; k < 8; k++){
            if(asc16_font[offset + i] & masks[k]) {
                fb_putpixel(fb, x + k, y + i, fgcolor);
            } else {
                uint32_t bg_img_color = bgcolor;
                if (fb->bg != NULL) {
                    bg_img_color = ((uint32_t*)(fb->bg + fb->pitch * y + x * 4))[0];
                }
                fb_putpixel(fb, x + k, y + i, bg_img_color);
            }
        }
    }
}

void fb_putlogo(fb_info_t *fb, uint32_t fgcolor, uint32_t bgcolor) 
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    char *logo = "HanOS";
    size_t len = strlen(logo);
    uint32_t x = (fb->width - len * 8 * LOGO_SCALE) / 2;
    uint32_t y = (fb->height - 16 * LOGO_SCALE) / 2;
    for (size_t idx = 0; idx < len; idx++) {
        uint32_t offset = ((uint32_t)logo[idx]) * 16; 
        static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
        for(size_t i = 0; i < 16; i++) {
            for(size_t k = 0; k < 8; k++) {
                for(size_t m = 1; m < LOGO_SCALE; m++) {
                    for(size_t n = 1; n < LOGO_SCALE; n++) {
                        uint32_t bg_img_color = bgcolor;
                        if (fb->bg != NULL) {
                            bg_img_color = ((uint32_t*)(fb->bg + fb->pitch * y + x * 4))[0];
                        } 
                        uint32_t color = (asc16_font[offset + i] & masks[k])
                            ? fgcolor : bg_img_color;
                        fb_putpixel(fb, x + (idx * 8 + k) * LOGO_SCALE + m,
                                    y + i * LOGO_SCALE + n, color);
                    }
                }
            }
        } /* End every character */
    }

    char desc_str[64] = "- A Hobby OS for x64 v";
    strcat(desc_str, VERSION);
    strcat(desc_str, " -");
    size_t desc_len = strlen(desc_str);   
    x = (fb->width - 8 * desc_len) / 2;
    for (size_t desc_idx = 0; desc_idx < desc_len; desc_idx++) {
        fb_putch(fb, x + 8 * desc_idx, (fb->height + 16 * LOGO_SCALE) / 2,
            COLOR_GREY, bgcolor, desc_str[desc_idx]);
    }
}

void fb_putpixel(fb_info_t *fb, uint32_t x, uint32_t y, uint32_t color)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x] = color;
    }

    fb->dirty_left = MIN(fb->dirty_left, x);
    fb->dirty_top  = MIN(fb->dirty_top, y);
    fb->dirty_right = MAX(fb->dirty_right, x);
    fb->dirty_bottom = MAX(fb->dirty_bottom, y);
}

uint32_t fb_getpixel(fb_info_t *fb, uint32_t x, uint32_t y)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return 0;

    if(fb->pitch * y + x * 4 < fb->backbuffer_len) {
        return ((uint32_t*)(fb->backbuffer + (fb->pitch * y)))[x];
    } else {
        return 0;
    }
}

void fb_init(fb_info_t *fb, struct limine_framebuffer* s)
{
    if(s == NULL) {
        if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) {
            fb->backbuffer = umalloc(fb->backbuffer_len);
            memcpy(fb->backbuffer, fb->addr, fb->backbuffer_len);
        }
        return;
    }

    fb->addr = (uint8_t*)s->address;
    fb->bg = NULL;
    fb->width = s->width;
    fb->height = s->height;
    fb->pitch = s->pitch;

    fb->backbuffer_len = fb->height * fb->pitch;
    fb->backbuffer = fb->addr;

    fb->dirty_left = fb->width;
    fb->dirty_top = fb->height;
    fb->dirty_right = 0;
    fb->dirty_bottom = 0;
    memset(&(fb->img_bg), sizeof(image_t), 0);;

    for(uint32_t x = 0; x < fb->width; x++) {
        for(uint32_t y = 0; y < fb->height; y++) {
            fb_putpixel(fb, x, y, DEFAULT_BGCOLOR);
        }
    }
    fb_refresh(fb, false);
}

void fb_refresh(fb_info_t *fb, bool forced)
{
    if((uint64_t)fb->addr != (uint64_t)fb->backbuffer) {
        fb_refresh_times++;
        uint64_t begin = hpet_get_nanos();

        uint64_t len = fb->backbuffer_len;
        if (fb->dirty_right >= fb->dirty_left
            && fb->dirty_bottom >= fb->dirty_top
            && !forced)
        {
            for (size_t i = fb->dirty_top; i <= fb->dirty_bottom; i++) {
                memcpy(fb->addr + (fb->pitch * i),
                       fb->backbuffer + (fb->pitch * i),
                       (fb->dirty_right - fb->dirty_left + 1) * 4);
            }
        } else {
            memcpy(fb->addr, fb->backbuffer, len);
        }

        fb->dirty_left = fb->width;
        fb->dirty_top = fb->height;
        fb->dirty_right = 0;
        fb->dirty_bottom = 0;

        uint64_t end = hpet_get_nanos();
        fb_refresh_nanos += end - begin;
    }
}

void fb_debug(void)
{
    klogi("FB: refresh %d times, avg %d nanos\n", fb_refresh_times,
          (fb_refresh_times > 0) ? (fb_refresh_nanos / fb_refresh_times) : 0);
}
