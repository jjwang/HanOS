/**-----------------------------------------------------------------------------

 @file    cloexectest.c
 @brief   Check that O_CLOEXEC descriptors are marked and closed on exec

 @details
 @verbatim

   Opens a file with O_CLOEXEC, verifies F_GETFD reports FD_CLOEXEC, then forks
   and execs itself. The child checks fd 3 after exec: the process server must
   have closed it, so F_GETFD on it fails.

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
    {"<help> cloexectest", "Check O_CLOEXEC is marked and closed on exec."},
};
/* *INDENT-ON* */

#define CLOEXEC_FD  3

int32_t main(int32_t argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        int32_t r = sys_fcntl(CLOEXEC_FD, F_GETFD, 0);

        if (r < 0)
            printf("cloexec: child fd closed ok\n");
        else
            printf("cloexec: child fd still open FAIL\n");
        sys_exit(r < 0 ? 0 : 1);
    }

    int32_t fd = sys_open("/bin/hansh", O_RDONLY | O_CLOEXEC);

    if (fd != CLOEXEC_FD) {
        printf("cloexec: open got fd %d FAIL\n", fd);
        sys_exit(1);
    }

    int32_t flags = sys_fcntl(fd, F_GETFD, 0);

    printf("cloexec: fd %d flags %d %s\n", fd, flags,
           (flags & FD_CLOEXEC) ? "ok" : "FAIL");

    char *cargv[] = { "cloexectest", "child", NULL };
    int32_t pid = sys_fork();

    if (pid == 0) {
        sys_exec("/bin/cloexectest", cargv);
        sys_exit(1);
    }

    sys_wait(pid);
    printf("cloexec: done\n");
    sys_exit((flags & FD_CLOEXEC) ? 0 : 1);
}
