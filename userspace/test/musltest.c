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
        int f = open("/bin/hansh", O_RDONLY);

        if (f >= 0) {
            int d = dup(f);

            if (d >= 0) {
                printf("dup fd=%d\n", d);
                close(d);
            } else {
                printf("dup FAIL errno=%d\n", errno);
            }
            close(f);
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
