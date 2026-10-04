/**-----------------------------------------------------------------------------

 @file    sigtest.c
 @brief   Deliver a signal to a process spinning in user mode

 @details
 @verbatim

   The child signals readiness over a pipe, then spins in user mode without a
   syscall. The parent sends SIGUSR1; the kernel must deliver it on the timer
   path, not only on a syscall return. A second child takes the default SIGTERM
   action and must die.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t got;

static void on_usr1(int32_t sig)
{
    got = sig;
}

/* Wait for the child to reach the spin loop (a byte on the pipe). */
static void wait_ready(int32_t fd)
{
    char c;

    while (read(fd, &c, 1) != 1)
        ;
}

/* Spin until the handler sets got. No syscall runs in the loop. */
static int32_t run_handler(void)
{
    int32_t pp[2];

    if (pipe(pp) != 0)
        return 1;

    pid_t pid = fork();

    if (pid == 0) {
        char c = 'r';

        signal(SIGUSR1, on_usr1);
        write(pp[1], &c, 1);
        while (got == 0)
            asm volatile ("" ::: "memory");
        _exit(got == SIGUSR1 ? 42 : 1);
    }
    if (pid < 0)
        return 1;

    wait_ready(pp[0]);
    close(pp[0]);
    close(pp[1]);

    kill(pid, SIGUSR1);

    int32_t st = 0;

    waitpid(pid, &st, 0);

    int32_t ok = WIFEXITED(st) && WEXITSTATUS(st) == 42;

    printf("sigtest: spin handler=%d status=0x%x\n", ok, st);
    return ok ? 0 : 1;
}

/* Spin with the default SIGTERM action; the timer path must kill it. */
static int32_t run_default(void)
{
    int32_t pp[2];

    if (pipe(pp) != 0)
        return 1;

    pid_t pid = fork();

    if (pid == 0) {
        char c = 'r';

        write(pp[1], &c, 1);
        while (1)
            asm volatile ("" ::: "memory");
    }
    if (pid < 0)
        return 1;

    wait_ready(pp[0]);
    close(pp[0]);
    close(pp[1]);

    kill(pid, SIGTERM);

    int32_t st = 0;

    waitpid(pid, &st, 0);

    int32_t sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;
    int32_t ok = (sig == SIGTERM);

    printf("sigtest: spin default signaled=%d sig=%d want=%d\n",
           WIFSIGNALED(st), sig, SIGTERM);
    return ok ? 0 : 1;
}

int32_t main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int32_t fail = 0;

    fail += run_handler();
    fail += run_default();

    printf("sigtest: %s\n", fail == 0 ? "PASS" : "FAIL");
    return fail;
}
