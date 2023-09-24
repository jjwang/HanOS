#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

char buf[512];

void cat(int fd)
{
    int n;

    while((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        if (sys_write(STDOUT, buf, n) != n) {
            printf("cat: write error\n");
            sys_exit(1);
        }
    }
    if(n < 0){
        printf("cat: read error\n");
        sys_exit(1);
    }
}

int main(int argc, char *argv[])
{
    int fd, i;

    if(argc <= 1) {
        cat(STDIN);
        sys_exit(0);
    }

    for(i = 1; i < argc; i++) {
        if((fd = sys_open(argv[i], O_RDONLY)) < 0) {
            printf("cat: cannot open ");
            printf(argv[i]);
            printf("\n");
            sys_exit(1);
        }
        cat(fd);
        sys_close(fd);
    }
    sys_exit(0);
}

