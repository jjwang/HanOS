/**-----------------------------------------------------------------------------

 @file    rm.c
 @brief   Implementation of the 'rm' command for HanOS userspace
 @details
 @verbatim

  This file provides the implementation of the 'rm' command which is used to
  remove files or directories in the HanOS operating system. It includes the
  main function that processes command-line arguments and calls the sys_unlink
  system function to delete specified files. Error handling is included for
  file deletion operations.

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
    {"<help> rm",       "Remove files or directories."},
};
/* *INDENT-ON* */

int main(int argc, char *argv[])
{
    int i;

    if (argc < 2) {
        fprintf(STDERR, "Usage: rm files...\n");
        sys_exit(1);
    }

    for (i = 1; i < argc; i++) {
        if (sys_unlink(argv[i]) < 0) {
            fprintf(STDERR, "rm: %s failed to delete\n", argv[i]);
            break;
        }
    }

    sys_exit(0);
}
