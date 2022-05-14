#include <fs/vfs.h>
#include <fs/fat32.h>
#include <lib/klog.h>
#include <test/test.h>

void file_test(void)
{
#if 0
    char* fn1 = "/disk/0/EFI/BOOT";
    vfs_handle_t f1 = vfs_open(fn1, VFS_MODE_READ);
    if (f1 != VFS_INVALID_HANDLE) {
        vfs_close(f1);
    } else {
        kloge("Open %s failed\n", fn1);
    }
#endif

    char* fn2 = "/disk/0/HELLOWLD.TXT";
    vfs_handle_t f2 = vfs_open(fn2, VFS_MODE_READWRITE);
    if (f2 != VFS_INVALID_HANDLE) {
        char buff_read[1024] = {0};
        char buff_write[1024] = "(1) This is a test -- END";
        vfs_write(f2, strlen(buff_write), buff_write);
        size_t readlen = vfs_read(f2, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn2, f2, buff_read);
        vfs_close(f2);
    } else {
        kloge("Open %s(%d) failed\n", fn2, f2);
    }

#if 1
    vfs_handle_t f21 = vfs_open(fn2, VFS_MODE_READWRITE);
    if (f21 != VFS_INVALID_HANDLE) {
        char buff_read[1800] = {0};
        char buff_write[1800] = "(2) This is a test";

        size_t m = strlen(buff_write);
        for (; m < 80; m++) {
            buff_write[m] = 'A';
        }
        buff_write[m] = 'B';

        vfs_seek(f21, 10);
        vfs_write(f21, strlen(buff_write), buff_write);
        vfs_seek(f21, 0);
        size_t readlen = vfs_read(f21, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn2, f21, buff_read);
        vfs_close(f21);
    } else {
        kloge("Open %s(%d) failed\n", fn2, f21);
    }
#endif

#if 0
    char* fn3 = "/disk/0/HANOS.TXT";
    vfs_handle_t f3 = vfs_open(fn3, VFS_MODE_READ);
    if (f3 != VFS_INVALID_HANDLE) {
        char buff_read[1024] = {0};
        size_t readlen = vfs_read(f3, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn3, f3, buff_read);

        vfs_close(f3);
    } else {
        kloge("Open %s failed\n", fn3);
    } 
#endif
}

