/**-----------------------------------------------------------------------------

 @file    crtest.c
 @brief   Create, write and re-read a file through the VFS server

 @details
 @verbatim

   Exercises the server's in-RAM files: create a new file, write a string,
   close it, reopen and read it back, then stat it. The initrd itself is
   read-only, so this only works if the server keeps a writable layer.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> crtest",   "Create and re-read a file through the VFS server."},
};
/* *INDENT-ON* */

int32_t main(int32_t argc, char *argv[])
{
    (void) argc;
    (void) argv;

    const char *path = "/root/ramfile";
    const char *msg = "hello from ramfs\n";
    int32_t fd;

    fd = sys_open(path, O_CREAT | O_WRONLY);
    if (fd < 0) {
        fprintf(STDERR, "crtest: create failed\n");
        sys_exit(1);
    }

    if (sys_write(fd, msg, strlen(msg)) != (int32_t) strlen(msg)) {
        fprintf(STDERR, "crtest: write failed\n");
        sys_close(fd);
        sys_exit(1);
    }
    sys_close(fd);

    fd = sys_open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(STDERR, "crtest: reopen failed\n");
        sys_exit(1);
    }

    char buf[64] = { 0 };
    int32_t n = sys_read(fd, buf, sizeof(buf) - 1);

    sys_close(fd);

    printf("crtest: read %d bytes: %s", n, buf);
    sys_exit(0);
}
