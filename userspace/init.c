#include <stddef.h>

#include <libc/string.h>
#include <libc/sysfunc.h>

char *argv[] = {
    "hansh",
    NULL
};

int main(void)
{
    int pid, wpid;

    /* TODO: Here we need to init console which can be found in XV6 */

    /* Loop to start shell program */
    for (;;) {
        printf("init: starting sh\n");
        pid = sys_fork();
        if(pid < 0) {
            printf("init: fork failed\n");
            sys_exit(1);
        } else if(pid == 0) {
            /* This is child process */
            sys_exec("/bin/hansh", argv);
            printf("init: exec sh failed\n");
            sys_exit(1);
        } else {
            /* This is parent process, note that child process id is pid. */
            sys_wait(-1);
        }
    }
}

