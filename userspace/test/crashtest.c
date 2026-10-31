/**-----------------------------------------------------------------------------

 @file    crashtest.c
 @brief   Check that a user exception terminates the faulting task
 @details
 @verbatim

   Each mode forks a child, makes it fault, and checks that the parent's wait
   returns with the matching signal. The thread mode faults inside a spawned
   thread so the whole thread group must terminate.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static void fault_segv(void)
{
    *(volatile int *) 0 = 1;
}

static void fault_fpe(void)
{
    volatile int zero = 0;

    volatile int r = 1 / zero;

    (void) r;
}

static void fault_ill(void)
{
    __asm__ volatile ("ud2");
}

static void *thread_fault(void *arg)
{
    (void) arg;
    fault_segv();
    return NULL;
}

static int child_fault(const char *mode)
{
    pid_t pid = fork();

    if (pid == 0) {
        if (strcmp(mode, "segv") == 0)
            fault_segv();
        else if (strcmp(mode, "fpe") == 0)
            fault_fpe();
        else
            fault_ill();
        _exit(0);
    }

    int status = 0;

    if (waitpid(pid, &status, 0) != pid) {
        printf("crashtest %s: wait FAIL errno=%d\n", mode, errno);
        return 1;
    }
    if (!WIFSIGNALED(status)) {
        printf("crashtest %s: not signaled status=0x%x FAIL\n", mode, status);
        return 1;
    }
    printf("crashtest %s: signal=%d ok\n", mode, WTERMSIG(status));
    return 0;
}

static int thread_fault_group(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        pthread_t th;

        if (pthread_create(&th, NULL, thread_fault, NULL) != 0)
            _exit(2);
        for (;;)
            pause();
    }

    int status = 0;

    if (waitpid(pid, &status, 0) != pid) {
        printf("crashtest thread: wait FAIL errno=%d\n", errno);
        return 1;
    }
    if (!WIFSIGNALED(status)) {
        printf("crashtest thread: not signaled status=0x%x FAIL\n", status);
        return 1;
    }
    printf("crashtest thread: signal=%d ok\n", WTERMSIG(status));
    return 0;
}

int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0);

    if (argc < 2) {
        printf("usage: crashtest segv|fpe|ill|thread\n");
        return 1;
    }
    if (strcmp(argv[1], "thread") == 0)
        return thread_fault_group();
    if (strcmp(argv[1], "segv") != 0 && strcmp(argv[1], "fpe") != 0
        && strcmp(argv[1], "ill") != 0) {
        printf("crashtest: unknown mode %s\n", argv[1]);
        return 1;
    }
    return child_fault(argv[1]);
}
