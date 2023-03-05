/**-----------------------------------------------------------------------------

 @file    fb.c
 @brief   Implementation of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer was got
  from Limine bootloader.

  History:
  Mar 27, 2022 - Rewrite fb_refresh() by memcpy() significantly improve the
                 frame rate. In the future, we should rewrite memcpy() by SSE
                 enhancements.
  Nov 25, 2022 - Background picture could be set by a bitmap file.

 @endverbatim
 @todo    Improve memcpy() by SSE enhancements.

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <version.h>
#include <sys/mm.h>
#include <sys/hpet.h>
#include <device/display/fb.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/lock.h>
#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/string.h>

#define LOGO_SCALE      6

extern font_psf1_t term_font_norm, term_font_bold;

bool fb_set_bg_image(fb_info_t *fb, image_t *img)
{
    if (img->bpp != 24) {
        return false;
    }

    memcpy(&(fb->img_bg), img, sizeof(image_t));
    if (fb->bgbuffer == NULL) {
        fb->bgbuffer = (uint8_t*)kmalloc(fb->width * fb->height * 4);
    }
    if (fb->swapbuffer == NULL) {
        fb->swapbuffer = (uint8_t*)kmalloc(fb->width * fb->height * 4);
    }

    for (size_t y = 0; y < fb->height; y++) {
        for (size_t x = 0; x < fb->width; x++) {
            size_t x2, y2;

            /* Determine the best-fit point's (x, y) in original bitmap */
            if (img->img_width == fb->width && img->img_height == fb->height) {
                x2 = x;
                y2 = y;
            } else {
                size_t x_new, y_new, x_rb, y_rb;
                x_new = x * 100 * img->img_width / fb->width;
                y_new = y * 100 * img->img_height / fb->height;
                x2 = MAX(MIN(DIV_ROUNDUP(x_new, 100), img->img_width), 1);
                y2 = MAX(MIN(DIV_ROUNDUP(y_new, 100), img->img_height), 1);

                uint64_t max_dist = (100 ^ 4), cur_dist;
                for (size_t k = 0; k < 2; k++) {
                    for (size_t m = 0; m < 2; m++) {
                        cur_dist = ((x_new - (x_rb - k) * 100) ^ 2)
                            + ((y_new - (y_rb - m) * 100) ^ 2);
                        if (cur_dist < max_dist) {
                            x2 = x_rb - k;
                            y2 = y_rb - m;
                            max_dist = cur_dist;
                        }
                    }
                }
            }

            size_t off = img->pitch * (img->img_height - 1 - y2);
            uint8_t *img_pixel = (uint8_t*)img->img + off + x2 * img->bpp / 8;
            uint8_t r, g, b;
            uint8_t shift = 2;

            b = img_pixel[0] >> shift;
            g = img_pixel[1] >> shift;
            r = img_pixel[2] >> shift;

            uint32_t color;
            color = (uint32_t)b + ((uint32_t)g << 8) + ((uint32_t)r << 16);
            ((uint32_t*)(fb->bgbuffer + (fb->pitch * y)))[x] = color;
        }
    }

    return true;
}

void fb_putch(fb_info_t *fb, uint32_t x, uint32_t y, 
              uint32_t fgcolor, uint32_t bgcolor, uint8_t ch)
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    uint32_t offset = ((uint32_t)ch) * term_font_norm.charsize;
    static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
    for (size_t i = 0; i < FONT_HEIGHT; i++) {
        for (size_t k = 0; k < FONT_WIDTH; k++) {
            if (i < term_font_norm.charsize
                && (term_font_norm.data[offset + i] & masks[k]))
            {
                fb_putpixel(fb, x + k, y + i, fgcolor);
            } else {
                fb_putpixel(fb, x + k, y + i, bgcolor);
            }
        }
    }
}

void fb_putlogo(fb_info_t *fb, uint32_t fgcolor, uint32_t bgcolor) 
{
    if((uint64_t)fb->addr == (uint64_t)fb->backbuffer) return;

    char *logo = "HNK";
    size_t len = strlen(logo);
    uint32_t x = (fb->width - len * 8 * LOGO_SCALE) / 2;
    uint32_t y = (fb->height - 16 * LOGO_SCALE) / 2;
    for (size_t idx = 0; idx < len; idx++) {
        uint32_t offset = ((uint32_t)logo[idx]) * term_font_bold.charsize; 
        static const uint8_t masks[8] = { 128, 64, 32, 16, 8, 4, 2, 1 };
        for(size_t i = 0; i < term_font_bold.charsize; i++) {
            for(size_t k = 0; k < 8; k++) {
                for(size_t m = 1; m < LOGO_SCALE; m++) {
                    for(size_t n = 1; n < LOGO_SCALE; n++) {
                        uint32_t bg_img_color = bgcolor;
                        if (fb->bgbuffer != NULL) {
                            bg_img_color = ((uint32_t*)(fb->bgbuffer + fb->pitch * y + x * 4))[0];
                        } 
                        uint32_t color = (term_font_bold.data[offset + i] & masks[k])
                            ? fgcolor : bg_img_color;
                        fb_putpixel(fb, x + (idx * 8 + k) * LOGO_SCALE + m,
                                    y + i * LOGO_SCALE + n, color);
                    }
                }
            }
        } /* End every character */
    }

    char desc_str[64] = "- Unix-like OS Kernel for x86-64 v";
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
            fb->backbuffer = kmalloc(fb->backbuffer_len);
            memcpy(fb->backbuffer, fb->addr, fb->backbuffer_len);
        }
        return;
    }

    fb->addr = (uint8_t*)s->address;
    fb->bgbuffer = NULL;
    fb->swapbuffer = NULL;
    fb->width = s->width;
    fb->height = s->height;
    fb->pitch = s->pitch;

    fb->backbuffer_len = fb->height * fb->pitch;
    fb->backbuffer = fb->addr;

    memset(&(fb->img_bg), sizeof(image_t), 0);;

    for(uint32_t x = 0; x < fb->width; x++) {
        for(uint32_t y = 0; y < fb->height; y++) {
            fb_putpixel(fb, x, y, DEFAULT_BGCOLOR);
        }
    }
    fb_refresh(fb);
}

void fb_refresh(fb_info_t *fb)
{
    if((uint64_t)fb->addr != (uint64_t)fb->backbuffer) {
        uint64_t len = fb->backbuffer_len;
        if (fb->bgbuffer == NULL) {
            memcpy(fb->addr, fb->backbuffer, len);
        } else {
            memcpy(fb->swapbuffer, fb->bgbuffer, len);
            uint32_t *src = (uint32_t*)fb->backbuffer;
            uint32_t *dst = (uint32_t*)fb->swapbuffer;
            size_t pt_num = len / 4;
            for (size_t i = 0; i < pt_num; i++) {
                if (src[i] != DEFAULT_BGCOLOR) dst[i] = src[i];
            }
            memcpy(fb->addr, fb->swapbuffer, len);
        }
    }
}

