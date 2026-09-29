/**-----------------------------------------------------------------------------

 @file    fb.h
 @brief   Definition of framebuffer related functions
 @details
 @verbatim

  Graphics can be displayed in a linear framebuffer - a simple array mapped
  in memory that represents the screen. The address of framebuffer  was got
  from Limine bootloader.

  The kernel only owns the framebuffer geometry and a back buffer here; the
  userspace console server maps the scan-out and draws the text.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <3rd-party/boot/limine.h>

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

/**
 * @brief Framebuffer geometry, scan-out address and back buffer
 */
typedef struct {
    uint8_t *addr;

    uint8_t *backbuffer;
    uint32_t backbuffer_len;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
} fb_info_t;

/* The single kernel framebuffer, filled from Limine and re-attached by the
 * display driver after a mode set. */
fb_info_t *fb_get(void);
void fb_init(fb_info_t *fb, struct limine_framebuffer *s);
void fb_refresh(fb_info_t *fb);

/* Boot splash: a wordmark and a progress bar shown by the kernel while the
 * userspace console server is still being loaded. */
void fb_splash(fb_info_t *fb);
void fb_splash_progress(fb_info_t *fb, uint32_t percent);
