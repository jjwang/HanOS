#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static void *thread_fn(void *arg)
{
    (void) arg;
    /* Exercise the shared fd table: write to stdout from the thread. */
    write(1, "thread-io\n", 10);
    return (void *) 42;
}

static volatile sig_atomic_t got_sig;

static void on_signal(int sig)
{
    got_sig = sig;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    errno = 0;
    int fd = open("/bin/hansh", O_RDONLY);

    if (fd >= 0) {
        char b[4];

        lseek(fd, 1, SEEK_SET);
        ssize_t n = read(fd, b, sizeof(b));

        printf("lseek+read=%ld first=%02x\n", (long) n, (unsigned char) b[0]);
        close(fd);
    }

    int p[2];

    if (pipe(p) == 0) {
        write(p[1], "pipe-ok", 7);
        char b[8] = { 0 };

        read(p[0], b, sizeof(b));
        printf("pipe=%s\n", b);
        close(p[0]);
        close(p[1]);
    } else {
        printf("pipe FAIL errno=%d\n", errno);
    }

    {
        int d = dup(1);

        if (d >= 0) {
            write(d, "dup-stdout\n", 11);
            printf("dup stdout fd=%d\n", d);
            close(d);
        } else {
            printf("dup(1) FAIL errno=%d\n", errno);
        }
    }

    {
        struct timespec ts = { 0, 2000000 };

        if (nanosleep(&ts, NULL) == 0)
            printf("nanosleep ok\n");
        else
            printf("nanosleep FAIL errno=%d\n", errno);
    }

    {
        struct timespec ts = { 0, 1000000 };

        if (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL) == 0)
            printf("clock_nanosleep ok\n");
        else
            printf("clock_nanosleep FAIL errno=%d\n", errno);
    }

    {
        struct pollfd pf = { 1, POLLOUT, 0 };
        int r = poll(&pf, 1, 0);

        printf("poll r=%d revents=0x%x\n", r, pf.revents);
    }

    {
        struct pollfd pf = { 0, POLLIN, 0 };
        int r = poll(&pf, 1, 50);

        printf("poll stdin r=%d\n", r);
    }

    printf("isatty=%d\n", isatty(1));

    {
        int s = socket(AF_INET, SOCK_DGRAM, 0);

        if (s >= 0) {
            struct sockaddr_in sa;
            struct sockaddr_in got;
            socklen_t gl = sizeof(got);

            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(12345);
            sa.sin_addr.s_addr = htonl(0x7f000001);

            if (bind(s, (struct sockaddr *) &sa, sizeof(sa)) == 0
                && getsockname(s, (struct sockaddr *) &got, &gl) == 0) {
                printf("sock port=%d ip=%08x\n", ntohs(got.sin_port),
                       ntohl(got.sin_addr.s_addr));

                struct pollfd pf;

                pf.fd = s;
                pf.events = POLLIN;
                pf.revents = 0;
                int before = poll(&pf, 1, 0);

                errno = 0;
                ssize_t sr = sendto(s, "hi", 2, 0,
                                    (struct sockaddr *) &sa, sizeof(sa));
                pf.revents = 0;
                int after = poll(&pf, 1, 0);

                printf("sockpoll before=%d after=%d revents=0x%x send=%ld "
                       "errno=%d\n", before, after, pf.revents, (long) sr,
                       errno);
            } else {
                printf("socket FAIL errno=%d\n", errno);
            }
            close(s);
        } else {
            printf("socket FAIL errno=%d\n", errno);
        }
    }

    {
        fd_set wf;
        struct timeval tv = { 0, 0 };

        FD_ZERO(&wf);
        FD_SET(1, &wf);

        int r = select(2, NULL, &wf, NULL, &tv);

        printf("select r=%d isset=%d\n", r, FD_ISSET(1, &wf) ? 1 : 0);
    }

    struct stat st;

    errno = 0;
    if (stat("/bin/hansh", &st) == 0)
        printf("stat size=%ld mode=%o\n", (long) st.st_size, st.st_mode);
    else
        printf("stat FAIL errno=%d\n", errno);
    DIR *d = opendir("/bin");

    if (d) {
        int n = 0;
        struct dirent *e;

        while ((e = readdir(d)) != NULL && n < 5) {
            printf("readdir=%s\n", e->d_name);
            n++;
        }
        closedir(d);
        printf("readdir count=%d\n", n);
    } else {
        printf("opendir FAIL errno=%d\n", errno);
    }

    pthread_t th;
    void *rv = NULL;

    errno = 0;
    int rc = pthread_create(&th, NULL, thread_fn, NULL);
    if (rc == 0) {
        pthread_join(th, &rv);
        printf("pthread rv=%ld\n", (long) rv);
    } else {
        printf("pthread_create FAIL rc=%d\n", rc);
    }

    {
        errno = 0;
        int mr = mkdir("/newdir", 0755);
        int sr = symlink("/bin/hansh", "/newdir/link");
        char linkbuf[128] = { 0 };
        ssize_t lr = readlink("/newdir/link", linkbuf, sizeof(linkbuf) - 1);
        int rr = rename("/newdir/link", "/newdir/link2");
        char linkbuf2[128] = { 0 };
        ssize_t lr2 =
            readlink("/newdir/link2", linkbuf2, sizeof(linkbuf2) - 1);
        struct stat lst;
        int st2 = lstat("/newdir/link2", &lst);

        printf("mkdir=%d symlink=%d readlink=%ld:%s rename=%d "
               "readlink2=%ld:%s lstat=%d mode=%o size=%ld\n", mr, sr,
               (long) lr, linkbuf, rr, (long) lr2, linkbuf2, st2, lst.st_mode,
               (long) lst.st_size);
    }

    {
        int pp[2];

        if (pipe(pp) == 0) {
            struct pollfd pf;
            int revents;

            pf.fd = pp[0];
            pf.events = POLLIN;
            pf.revents = 0;
            int r0 = poll(&pf, 1, 0);

            write(pp[1], "x", 1);
            pf.revents = 0;
            int r1 = poll(&pf, 1, 0);
            revents = pf.revents;
            pf.fd = pp[1];
            pf.events = POLLOUT;
            pf.revents = 0;
            int r2 = poll(&pf, 1, 0);

            printf("pipepoll empty=%d data=%d revents=0x%x wr=%d\n", r0, r1,
                   revents, r2);
            close(pp[0]);
            close(pp[1]);
        } else {
            printf("pipepoll FAIL errno=%d\n", errno);
        }
    }

    {
        int ev = eventfd(0, EFD_NONBLOCK);

        if (ev >= 0) {
            uint64_t v = 5;
            int wr = (int) write(ev, &v, 8);
            uint64_t rv = 0;
            int rd = (int) read(ev, &rv, 8);

            printf("eventfd wr=%d rd=%d val=%llu\n", wr, rd,
                   (unsigned long long) rv);
            close(ev);
        } else {
            printf("eventfd FAIL errno=%d\n", errno);
        }
    }

    {
        int ep = epoll_create1(0);
        int ev2 = eventfd(0, EFD_NONBLOCK);

        if (ep >= 0 && ev2 >= 0) {
            struct epoll_event ee;
            struct epoll_event out[4];

            memset(&ee, 0, sizeof(ee));
            ee.events = EPOLLIN;
            ee.data.u64 = 123;

            int cr = epoll_ctl(ep, EPOLL_CTL_ADD, ev2, &ee);
            int w0 = epoll_wait(ep, out, 4, 0);
            uint64_t v = 1;

            write(ev2, &v, 8);
            int w1 = epoll_wait(ep, out, 4, 100);

            printf("epoll ctl=%d empty=%d ready=%d events=0x%x data=%llu\n",
                   cr, w0, w1, (w1 > 0) ? out[0].events : 0,
                   (unsigned long long) ((w1 > 0) ? out[0].data.u64 : 0));
            close(ev2);
            close(ep);
        } else {
            printf("epoll FAIL ep=%d ev=%d errno=%d\n", ep, ev2, errno);
        }
    }

    {
        struct sigaction sa;
        sigset_t blk, old;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = on_signal;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGUSR1, &sa, NULL);
        sigaction(SIGUSR2, &sa, NULL);

        got_sig = 0;
        errno = 0;
        int rr = raise(SIGUSR1);
        printf("signal raise ret=%d errno=%d got=%d\n", rr, errno,
               (int) got_sig);

        got_sig = 0;
        errno = 0;
        int kr = kill(getpid(), SIGUSR1);
        printf("signal kill ret=%d errno=%d got=%d\n", kr, errno,
               (int) got_sig);

        got_sig = 0;
        sigemptyset(&blk);
        sigaddset(&blk, SIGUSR2);
        sigprocmask(SIG_BLOCK, &blk, &old);
        raise(SIGUSR2);
        printf("signal blocked got=%d\n", (int) got_sig);
        sigprocmask(SIG_SETMASK, &old, NULL);
        printf("signal unblocked got=%d\n", (int) got_sig);
    }

    {
        pid_t pid = fork();

        if (pid == 0) {
            /* Write an unmapped address. The kernel kills this process and
             * the parent keeps running. */
            *(volatile unsigned long *) 0x12345000UL = 1;
            _exit(0);
        } else if (pid > 0) {
            int st = 0;

            waitpid(pid, &st, 0);
            printf("faultkill signaled=%d sig=%d\n", WIFSIGNALED(st),
                   WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        } else {
            printf("faultkill FAIL errno=%d\n", errno);
        }
    }

    printf("mustest: done\n");
    return 0;
}
