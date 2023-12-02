#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

static command_help_t help_msg[] = { 
    {"<help> wc",       "Print newline, word, and byte counts."},
};

char buf[512] = {0};

void wc(int fd, char *name)
{
    int i, n;
    int l, w, c, inword;

    l = w = c = 0;
    inword = 0;
    /* Read one character and write back to screen everytime */
    while((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        for(i = 0; i < n; i++) {
            if(buf[i] == (char)EOF) {
                goto succ_exit;
            }
            c++;
            if(buf[i] == '\n') {
                l++;
            }
            if(strchr(" \r\t\n\v", buf[i])) {
                inword = 0;
            } else {
                if (inword == 0) w++;
                inword = 1;
            }
        }
    }
    if(n < 0) {
        printf("wc: read error\n");
        sys_exit(1);
    }
succ_exit:
    if(inword) {
        l++;
        w++;
    } 
    printf("\t%d\t%d\t%d\n", l, w, c);
}

int main(int argc, char *argv[])
{
    int fd, i;

    if(argc <= 1) {
        wc(STDIN, "[unknown]");
        sys_exit(0);
    }

    for(i = 1; i < argc; i++) {
        if((fd = sys_open(argv[i], 0)) < 0){
            printf("wc: cannot open \n");
            printf(argv[i]);
            printf("\n");
            sys_exit(1);
        }
        wc(fd, argv[i]);
        sys_close(fd);
    }
    sys_exit(0);
}

