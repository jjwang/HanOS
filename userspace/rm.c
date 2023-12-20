#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

static command_help_t help_msg[] = {
    {"<help> rm",       "Remove files or directories."},
};

int main(int argc, char *argv[])
{
    int i;

    if(argc < 2) {
        fprintf(STDERR, "Usage: rm files...\n");
        sys_exit(1);
    }

    for(i = 1; i < argc; i++) {
        if(sys_unlink(argv[i]) < 0) {
            fprintf(STDERR, "rm: %s failed to delete\n", argv[i]);
            break;
        }
    }

    sys_exit(0);
}

