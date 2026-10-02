/**-----------------------------------------------------------------------------

 @file    echo.c
 @brief   Implementation of the 'echo' command for HanOS userspace
 @details
 @verbatim

  This file provides the implementation of the 'echo' command which is used to
  display a specified string. It includes the main function to handle
command-line
  arguments and output them to the standard output, separated by spaces and
followed
  by a newline. This command is useful for displaying messages or concatenating
  command-line arguments.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stddef.h>

#include <string.h>
#include <sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = { 
    {"<help> echo",     "Display a specified string."},
};
/* *INDENT-ON* */

int32_t main(int32_t argc, char *argv[])
{
    int32_t i;
    char msg[3] = { 0 };

    for (i = 1; i < argc; i++) {
        sys_write(STDOUT, argv[i], strlen(argv[i]));
        if (i + 1 < argc) {
            sys_write(STDOUT, " ", 1);
        } else {
            sys_write(STDOUT, "\n", 1);
        }
    }

    return 0;
}
