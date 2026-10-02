/**-----------------------------------------------------------------------------

 @file    fdtest.c
 @brief   Check that a server-backed fd is shared correctly across fork

 @details
 @verbatim

   Opens a file through the VFS server and forks several times. Parent and
   child share one open file description, so each read advances the same
   offset; every read must succeed. A failure means the server-side
   reference was taken too late (or not at all) and the fd was freed under
   one of the readers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <string.h>
#include <sysfunc.h>

#define FD_FORKS    4

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> fdtest",   "Read a server fd from forked children and the parent."},
};
/* *INDENT-ON* */

int32_t main(int32_t argc, char *argv[])
{
    (void) argc;
    (void) argv;

    int32_t fd = sys_open("/bin/hansh", 0);
    if (fd < 0) {
        fprintf(STDERR, "fdtest: open /bin/hansh failed\n");
        sys_exit(1);
    }

    for (int32_t i = 0; i < FD_FORKS; i++) {
        int32_t pid = sys_fork();

        if (pid == 0) {
            uint8_t b = 0;
            int32_t n = sys_read(fd, &b, 1);

            printf("fdtest[%d] child  %s %02x\n", i, (n == 1) ? "ok" : "FAIL",
                   b);
            sys_exit(0);
        }

        if (pid < 0) {
            fprintf(STDERR, "fdtest: fork failed\n");
            sys_exit(1);
        }

        sys_wait(pid);

        uint8_t b = 0;
        int32_t n = sys_read(fd, &b, 1);

        printf("fdtest[%d] parent %s %02x\n", i, (n == 1) ? "ok" : "FAIL", b);
    }

    sys_exit(0);
}
