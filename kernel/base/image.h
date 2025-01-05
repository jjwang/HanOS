/**-----------------------------------------------------------------------------

 @file    image.h
 @brief   Image handling definitions
 @details
 @verbatim

  This file contains the type definitions and function declarations for image
  handling within the HanOS kernel. It includes the structure definition for
  representing an image and a function prototype for loading a BMP image from
  a file.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct [[gnu::packed]] {
    uint8_t     *img;;
    uint64_t    size;
    uint64_t    pitch;
    uint64_t    bpp;
    uint64_t    img_width;
    uint64_t    img_height;
} image_t;

bool bmp_load_from_file(image_t *image, char *fn);

