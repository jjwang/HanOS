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

    for (int i = 0; i < 50; i++) {
        if (shell_file("/bin/bash")) {
            *name = "bash";
            return "/bin/bash";
        }
        if (shell_file("/data/bin/bash")) {
            *name = "bash";
            return "/data/bin/bash";
        }
        /* The ext2 mount exposes /data/bin in one step. Once it is there,
         * the absence of bash is final. */
        if (shell_file("/data/bin"))
            break;
        nanosleep(&delay, NULL);
    }

    *name = "hansh";
    return "/bin/hansh";
}

int32_t main(void)
{
    int32_t pid, wpid;

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
