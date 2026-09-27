/**-----------------------------------------------------------------------------

 @file    fdtest.c
 @brief   Check that a server-backed fd is shared correctly across fork

 @details
 @verbatim

   Opens a file through the VFS server, forks, and reads from both the child
   and the parent. The two processes share one open file description, so the
   parent's read continues where the child's stopped; both reads must succeed
   and neither close may invalidate the other's fd.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> fdtest",   "Read a server fd from a forked child and the parent."},
};
/* *INDENT-ON* */

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    int fd = sys_open("/bin/hansh", 0);
    if (fd < 0) {
        fprintf(STDERR, "fdtest: open /bin/hansh failed\n");
        sys_exit(1);
    }

    int pid = sys_fork();

    if (pid == 0) {
        unsigned char b[4] = { 0 };
        int n = sys_read(fd, b, sizeof(b));

        printf("fdtest child read %d: %02x %02x %02x %02x\n", n, b[0], b[1],
               b[2], b[3]);
        sys_exit(0);
    }

    if (pid > 0) {
        unsigned char b[4] = { 0 };
        int n;

        sys_wait(pid);
        n = sys_read(fd, b, sizeof(b));
        printf("fdtest parent read %d: %02x %02x %02x %02x\n", n, b[0], b[1],
               b[2], b[3]);
        sys_exit(0);
    }

    fprintf(STDERR, "fdtest: fork failed\n");
    sys_exit(1);
}
