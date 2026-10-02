/**-----------------------------------------------------------------------------

 @file    help.c
 @brief   Implementation of the 'help' command

 @details
 @verbatim

   Prints the shell's command help table. The table itself is generated into
   _help.c; this file provides a weak default table so the binary links even
   before the table is generated.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <userspace/help.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = { 
    {"<help> help",     "Print all available commands."},
};
/* *INDENT-ON* */

[[gnu::weak]]
const command_help_t _shell_helptab[] = {
    { "", "" }
};

void main(int32_t argc, char *argv[])
{
    for (int32_t i = 0;; i++) {
        if (strlen(_shell_helptab[i].command) == 0)
            break;
        printf("%s\t%s\n", &(_shell_helptab[i].command[7]),
               _shell_helptab[i].desc);
    }
}
