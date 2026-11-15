/**-----------------------------------------------------------------------------

 @file    usb.c
 @brief   USB mass-storage block system calls

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <lib/kmalloc.h>
#include <mm/uaccess.h>
#include <device/usb/xhci.h>
#include <proc/syscall.h>

#define USB_BLOCK_MAX_SECTORS   128

int64_t k_usb_block_info(void)
{
    if (!usb_msc_wait(8000)) {
        cpu_set_errno(ENODEV);
        return -1;
    }
    cpu_set_errno(0);
    return (int64_t) usb_msc_sectors();
}

int64_t k_usb_block_read(int64_t lba, int64_t count, int64_t uptr)
{
    if (!usb_msc_ready() || lba < 0 || count <= 0
        || count > USB_BLOCK_MAX_SECTORS) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    uint64_t bytes = (uint64_t) count * 512;
    uint8_t *kbuf = kmalloc(bytes);

    if (kbuf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }
    if (!usb_msc_read((uint32_t) lba, (uint32_t) count, kbuf)) {
        kmfree(kbuf);
        cpu_set_errno(EIO);
        return -1;
    }
    int32_t rc = (int32_t) copy_to_user((void *) uptr, kbuf, bytes);

    kmfree(kbuf);
    if (rc != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    cpu_set_errno(0);
    return (int64_t) bytes;
}

int64_t k_usb_block_write(int64_t lba, int64_t count, int64_t uptr)
{
    if (!usb_msc_ready() || lba < 0 || count <= 0
        || count > USB_BLOCK_MAX_SECTORS) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    uint64_t bytes = (uint64_t) count * 512;
    uint8_t *kbuf = kmalloc(bytes);

    if (kbuf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }
    if (copy_from_user(kbuf, (const void *) uptr, bytes) != 0) {
        kmfree(kbuf);
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (!usb_msc_write((uint32_t) lba, (uint32_t) count, kbuf)) {
        kmfree(kbuf);
        cpu_set_errno(EIO);
        return -1;
    }
    kmfree(kbuf);
    cpu_set_errno(0);
    return (int64_t) bytes;
}
