#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

#define DIRSIZE     256

static command_help_t help_msg[] = {
    {"<help> ls",       "List the contents of a specified directory."},
};

char *fmtname(char *path, char *buf)
{
    char *p;

    /* Find first character after last slash. */
    for(p = path + strlen(path); p >= path && *p != '/'; p--)
        ;
    p++;

    /* Return blank-padded name. */
    if(strlen(p) >= DIRSIZE)
        return p;
    memcpy(buf, p, strlen(p));
    memset(buf + strlen(p), ' ', DIRSIZE - strlen(p));
    buf[strlen(p)] = '\0';
    return buf;
}

void ls(char *path)
{
    /* TODO: We should check buffer length later */
    char fmtbuf[DIRSIZE + 1] = {0};
    char buf[DIRSIZE + 1];
    char *p;
    int fd, num;
    dirent_t de;
    stat_t st;

    if(strcmp(path, ".") == 0) {
        if(sys_getcwd(buf, sizeof(buf) - 1) < 0) {
            printf("ls: getcwd failed\n");
            sys_exit(0);
        }
    }
    printf("Files in \"%s\" folder:\n", buf);

    if((fd = sys_open(path, 0)) < 0){
        printf("ls: cannot open %s\n", path);
        return;
    }

    if(sys_fstat(fd, &st) < 0) {
        printf("ls: cannot stat %s\n", path);
        sys_close(fd);
        return;
    }

    num = 0;

    switch(st.st_mode & S_IFMT) {
    case S_IFDIR:
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';
        while(sys_readdir(fd, &de) >= 0) {
            if(de.d_ino == 0)
                continue;
            memcpy(p, de.d_name, sizeof(de.d_name));
            p[sizeof(de.d_name)] = 0;
            if(sys_stat(buf, &st) < 0) {
                printf("ls: cannot stat %s\n", buf);
                continue;
            }
            printf("%s\t0x%x\t%d\t%d\n", fmtname(buf, fmtbuf),
                   (st.st_mode & S_IFMT) >> 12, st.st_ino, st.st_size);
            num++;
        }
        if (num == 0) printf("ls: no files found\n");
        break;
    default:
        printf("ls: \"%s\" is not a folder (0x%x)\n", path,
               (st.st_mode & S_IFMT) >> 12);
        break;
    }
    sys_close(fd);
}

int main(int argc, char *argv[])
{
    int i;

    if(argc < 2) {
        ls(".");
        sys_exit(0);
    }
    for(i = 1; i < argc; i++)
        ls(argv[i]);
    sys_exit(0);
}

