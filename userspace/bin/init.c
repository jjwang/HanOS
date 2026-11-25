/**-----------------------------------------------------------------------------

 @file    init.c
 @brief   Initialization process for HanOS userspace
 @details
 @verbatim

  This file provides the implementation of the initialization process for the
  HanOS userspace. It includes the main function that continuously starts the
  shell by forking and executing the shell program. bash runs when the image
  carries it; otherwise init starts hansh. The init process is responsible for
  ensuring that the shell is always running and handles the necessary process
  management.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sysfunc.h>

static bool shell_file(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
}

static const char *shell_path(const char **name)
{
    struct timespec delay = { 0, 10 * 1000 * 1000 };

    /* The VFS server may register after init starts. Wait for the initrd
     * namespace before the bash lookup. */
    for (int i = 0; i < 100; i++) {
        if (shell_file("/bin/hansh"))
            break;
        nanosleep(&delay, NULL);
    }

    /* A ports image carries /etc/ports-image in the initrd. Wait for the
     * requested bash until the data partition is mounted, so login can find
     * it. */
    if (shell_file("/etc/ports-image")) {
        for (;;) {
            if (shell_file("/bin/bash") || shell_file("/data/bin/bash"))
                break;
            nanosleep(&delay, NULL);
        }
        /* The ports image carries a shadow store, so authenticate first and
         * let login pick the shell. */
        *name = "login";
        return "/bin/login";
    }

    /* A plain image has no shadow store; start the shell directly. */
    *name = "hansh";
    return "/bin/hansh";
}

int32_t main(void)
{
    int32_t pid, wpid;

    /* On a ports image, start the Wayland compositor once the data partition
     * (and its shared libraries) is mounted. It runs in the background. */
    if (shell_file("/etc/ports-image")) {
        struct timespec delay = { 0, 10 * 1000 * 1000 };

        for (int i = 0; i < 600; i++) {
            if (shell_file("/data/bin/compositor"))
                break;
            nanosleep(&delay, NULL);
        }
        if (shell_file("/data/bin/compositor") && sys_fork() == 0) {
            char *av[2] = { "/data/bin/compositor", NULL };
            int lfd = open("/data/compositor.log",
                           O_WRONLY | O_CREAT | O_APPEND, 0644);

            /* Keep the console and login prompt clean; compositor messages
             * go to its log file. */
            if (lfd >= 0) {
                dup2(lfd, 1);
                dup2(lfd, 2);
                if (lfd > 2)
                    close(lfd);
            }
            sys_exec("/data/bin/compositor", av);
            sys_exit(1);
        }
    }

    /* Loop to start shell program */
    for (;;) {
        const char *name;
        char *argv[2];

        printf("init: starting sh...type \"help\" for command list\n");
        pid = sys_fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            sys_exit(1);
        } else if (pid == 0) {
            /* This is child process */
            const char *path = shell_path(&name);

            argv[0] = (char *) name;
            argv[1] = NULL;
            sys_exec(path, argv);
            printf("init: exec sh failed\n");
            sys_exit(1);
        } else {
            /* This is parent process, note that child process id is pid. */
            sys_wait(-1);
        }
    }
    return 0;
}
