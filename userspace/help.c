/* 
 * This file is a placeholder
 * DO NOT EDIT
 */

#include <libc/string.h>
#include <userspace/help.h>

static command_help_t help_msg[] = { 
    {"<help> help",     "Print all available commands."},
};

[[gnu::weak]] const command_help_t _shell_helptab[] = {
    {"", ""}
};

void main(int argc, char *argv[])
{
    for (size_t i = 0; ;i++) {
        if (strlen(_shell_helptab[i].command) == 0) break;
        printf("%s\t%s\n", &(_shell_helptab[i].command[7]),
               _shell_helptab[i].desc);
    }
}

