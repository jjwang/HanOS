/**-----------------------------------------------------------------------------

 @file    faulttest.c
 @brief   Trigger user-mode CPU exceptions and check the signal

 @details
 @verbatim

   Forks one child per fault: a store to an unmapped address (SIGSEGV), a
   divide by zero (SIGFPE) and an invalid opcode (SIGILL). The kernel must
   kill each child with that signal while the parent keeps running.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *sig_name(int32_t sig)
{
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV";
    case SIGFPE:
        return "SIGFPE";
    case SIGILL:
        return "SIGILL";
    case SIGBUS:
        return "SIGBUS";
    default:
        return "?";
    }
}

static void fault_page(void)
{
    *(volatile unsigned long *) 0x12345000UL = 1;
}

static void fault_divide(void)
{
    volatile int32_t zero = 0;
    volatile int32_t r = 1 / zero;

    (void) r;
}

static void fault_opcode(void)
{
    asm volatile ("ud2");
}

/* Fork a child that runs fn, then report the signal that kills it. */
static int32_t run_fault(const char *name, int32_t want, void (*fn)(void))
{
    pid_t pid = fork();

    if (pid == 0) {
        fn();
        _exit(0);               /* the fault must not return */
    }
    if (pid < 0) {
        printf("faulttest: %s fork FAIL errno=%d\n", name, errno);
        return 1;
    }

    int32_t st = 0;

    waitpid(pid, &st, 0);

    int32_t sig = WIFSIGNALED(st) ? WTERMSIG(st) : 0;

    printf("faulttest: %s signaled=%d sig=%d(%s) want=%d(%s)\n", name,
           WIFSIGNALED(st), sig, sig_name(sig), want, sig_name(want));

    return (sig == want) ? 0 : 1;
}

int32_t main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    int32_t fail = 0;

    fail += run_fault("page", SIGSEGV, fault_page);
    fail += run_fault("divide", SIGFPE, fault_divide);
    fail += run_fault("opcode", SIGILL, fault_opcode);

    printf("faulttest: %s\n", fail == 0 ? "PASS" : "FAIL");
    return fail;
}
