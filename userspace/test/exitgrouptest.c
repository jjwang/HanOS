#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void *thread_exit(void *arg)
{
    (void) arg;
    /* exit() calls exit_group; this thread is not the group leader. */
    exit(42);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    pid_t pid = fork();

    if (pid == 0) {
        pthread_t th;

        if (pthread_create(&th, NULL, thread_exit, NULL) != 0)
            _exit(1);
        for (;;)
            pause();
    }

    int status = 0;

    if (waitpid(pid, &status, 0) != pid) {
        printf("exitgrouptest wait FAIL errno=%d\n", errno);
        return 1;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
        printf("exitgrouptest PASS status=%d\n", WEXITSTATUS(status));
        return 0;
    }

    printf("exitgrouptest FAIL status=0x%x\n", status);
    return 1;
}
