/**-----------------------------------------------------------------------------

 @file    initrd.c
 @brief   Read files from the boot initrd image

 @details
 @verbatim

   Parses the ustar archive the bootloader passes as the INITRD module. The
   kernel uses it only to load the first userspace servers; after the VFS
   server registers, paths are served from user space.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <fs/initrd.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>

#define USTAR_BLOCK         512
#define USTAR_NAME_OFF      0
#define USTAR_NAME_LEN      100
#define USTAR_SIZE_OFF      0x7c
#define USTAR_SIZE_LEN      12
#define USTAR_TYPE_OFF      0x9c
#define USTAR_MAGIC_OFF     257
#define USTAR_TYPE_FILE     '0'
#define USTAR_TYPE_FILE_OLD '\0'

static uint8_t *initrd_address = NULL;
static uint64_t initrd_size = 0;

void initrd_init(void *address, uint64_t size)
{
    initrd_address = (uint8_t *) address;
    initrd_size = size;
}

void initrd_get(void **address, uint64_t *size)
{
    if (address != NULL)
        *address = initrd_address;
    if (size != NULL)
        *size = initrd_size;
}

static uint64_t oct2bin(const uint8_t * str, uint64_t len)
{
    uint64_t n = 0;

    while (len-- > 0 && *str >= '0' && *str <= '7') {
        n = n * 8 + (*str - '0');
        str++;
    }
    return n;
}

/* Compare an archive entry name with a requested path, ignoring a leading
 * "./" or "/". */
static bool name_matches(const uint8_t * name, uint64_t name_len,
                         const char *path)
{
    while (name_len > 0 && name[name_len - 1] == '\0')
        name_len--;
    if (name_len >= 2 && name[0] == '.' && name[1] == '/') {
        name += 2;
        name_len -= 2;
    }
    while (name_len > 0 && name[0] == '/') {
        name++;
        name_len--;
    }
    while (path[0] == '/')
        path++;

    return (uint64_t) strlen(path) == name_len
        && memcmp(name, path, name_len) == 0;
}

int64_t initrd_load(const char *path, uint8_t **out_buf, uint64_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    if (initrd_address == NULL || path == NULL)
        return -1;

    uint8_t *ptr = initrd_address;
    uint8_t *end = initrd_address + initrd_size;

    while (ptr + USTAR_BLOCK <= end
           && memcmp(ptr + USTAR_MAGIC_OFF, "ustar", 5) == 0) {
        uint64_t fsize = oct2bin(ptr + USTAR_SIZE_OFF, USTAR_SIZE_LEN);
        uint8_t type = ptr[USTAR_TYPE_OFF];

        if ((type == USTAR_TYPE_FILE || type == USTAR_TYPE_FILE_OLD)
            && name_matches(ptr + USTAR_NAME_OFF, USTAR_NAME_LEN, path)) {
            uint8_t *buf = NULL;

            if (fsize > 0) {
                buf = (uint8_t *) kmalloc_chunk(fsize, __func__, __LINE__);
                if (buf == NULL)
                    return -1;
                if (ptr + USTAR_BLOCK + fsize > end) {
                    kmfree(buf);
                    return -1;
                }
                memcpy(buf, ptr + USTAR_BLOCK, fsize);
            }
            *out_buf = buf;
            *out_len = fsize;
            return 0;
        }

        ptr += USTAR_BLOCK + ((fsize + USTAR_BLOCK - 1) & ~(uint64_t) (USTAR_BLOCK - 1));
    }

    return -1;
}
