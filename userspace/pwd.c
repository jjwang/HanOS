#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

#define DIRSIZE     256

static command_help_t help_msg[] = { 
    {"<help> pwd",      "Print current directory."},
};

int main(int argc, char *argv[])
{
    char path[DIRSIZE + 1] = {0};
    int ret = sys_getcwd(path, DIRSIZE);
    if (ret < 0) {
        printf("pwd: getting current workding folder failed\n");
    } else {
        printf("%s\n", path);
    }
}

