/**-----------------------------------------------------------------------------

 @file    image.c
 @brief   Implementation of image handling functions
 @details
 @verbatim

  This file contains the implementation of functions for handling images within
  the HanOS kernel. It includes the function for loading BMP images from a file
  into an image structure. The BMP loading function reads the file, parses the
  BMP header, and extracts the image data for use within the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <fs/vfs.h>
#include <lib/klib.h>
#include <lib/kmalloc.h>
#include <lib/image.h>

typedef struct[[gnu::packed]] {
    uint16_t bf_signature;
    uint32_t bf_size;
    uint32_t reserved;
    uint32_t bf_offset;

    uint32_t bi_size;
    uint32_t bi_width;
    uint32_t bi_height;
    uint16_t bi_planes;
    uint16_t bi_bpp;
    uint32_t bi_compression;
    uint32_t bi_image_size;
    uint32_t bi_xcount;
    uint32_t bi_ycount;
    uint32_t bi_clr_used;
    uint32_t bi_clr_important;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
} bmp_header_t;

bool bmp_load_from_file(image_t * image, char *fn)
{
    uint64_t bmp_size = 0;
    uint8_t *bmp_buff = NULL;

    if (vfs_load_file(fn, &bmp_buff, &bmp_size) != 0 || bmp_buff == NULL) {
        klogi("bmp: cannot load %s\n", fn);
        return false;
    }

    bmp_header_t header;
    memcpy(&header, bmp_buff, sizeof(bmp_header_t));
    if (memcmp(&header.bf_signature, "BM", 2) != 0)
        return false;

    /* Don't support bpp lower than 8 */
    if (header.bi_bpp % 8 != 0) {
        kmfree_chunk(bmp_buff, __func__, __LINE__);
        return false;
    }

    uint32_t bf_size;
    if (header.bf_offset + header.bf_size > bmp_size) {
        bf_size = bmp_size - header.bf_offset;
    } else {
        bf_size = header.bf_size;
    }

    image->img = (uint8_t *) kmalloc(bf_size);
    if (image->img != NULL) {
        memcpy(image->img, bmp_buff + header.bf_offset, bf_size);
    }

    image->size = header.bf_size;
    image->pitch = ALIGNUP(header.bi_width * header.bi_bpp, 32) / 8;
    image->bpp = header.bi_bpp;
    image->img_width = header.bi_width;
    image->img_height = header.bi_height;

    kmfree_chunk(bmp_buff, __func__, __LINE__);

    return image->img != NULL ? true : false;
}
