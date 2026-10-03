#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <sys/select.h>
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

    printf("mustest: done\n");
    return 0;
}
