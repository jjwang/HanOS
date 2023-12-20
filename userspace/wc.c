#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

static command_help_t help_msg[] = { 
    {"<help> wc",       "Print newline, word, and byte counts."},
};

char buf[512] = {0};
char out[512] = {0};
bool outmore = false;

void wc(int fd, char *name)
{
    int i, n;
    int l, w, c, inword;

    l = w = c = 0;
    inword = 0;
    /* Read one character and write back to screen everytime */
    while((n = sys_read(fd, buf, sizeof(buf) - 1)) > 0) {
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
        buf[n] = '\0';
        if (strlen(buf) + strlen(out) < sizeof(out) - 1) {
            strcat(out, buf);
            outmore = false;
        } else {
            outmore = true;
        }
    }
    if(n < 0) {
        fprintf(STDERR, "wc: read error\n");
        sys_exit(1);
    }
succ_exit:
    if(inword) {
        l++;
        w++;
    } 
    printf("\t%d\t%d\t%d\n%s%s\n", l, w, c,
           out, outmore ? " ..." : "[EOF]");
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
            fprintf(STDERR, "wc: cannot open \n");
            fprintf(STDERR, argv[i]);
            fprintf(STDERR, "\n");
            sys_exit(1);
        }
        wc(fd, argv[i]);
        sys_close(fd);
    }
    sys_exit(0);
}

