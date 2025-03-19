/**-----------------------------------------------------------------------------

 @file    cat.c
 @brief   Implementation of the 'cat' command for HanOS userspace
 @details
 @verbatim

  This file provides the implementation of the 'cat' command which is used to
  concatenate and display files on the standard output. It includes the main
  function to handle command-line arguments and the cat function for reading
  from a file descriptor and writing to standard output. Error handling is
  included for read and write operations.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <libc/stdio.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = { 
    {"<help> cat",      "Concatenate files and print on the standard output."},
};
/* *INDENT-ON* */

char buf[512] = { 0 };

void cat(int fd)
{
    int n;

    while ((n = sys_read(fd, buf, sizeof(buf))) > 0) {
        if (sys_write(STDOUT, buf, n) != n) {
            fprintf(STDERR, "cat: write error\n");
            sys_exit(1);
        }
    }
    if (n < 0) {
        fprintf(STDERR, "cat: read error\n");
        sys_exit(1);
    }
}

int main(int argc, char *argv[])
{
    int fd, i;

    if (argc <= 1) {
        cat(STDIN);
        sys_exit(0);
    }

    for (i = 1; i < argc; i++) {
        if ((fd = sys_open(argv[i], O_RDONLY)) < 0) {
            fprintf(STDERR, "cat: cannot open ");
            fprintf(STDERR, argv[i]);
            fprintf(STDERR, "\n");
            sys_exit(1);
        }
        cat(fd);
        sys_close(fd);
    }
    sys_exit(0);
}
