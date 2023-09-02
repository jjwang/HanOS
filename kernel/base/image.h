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

