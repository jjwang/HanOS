/**-----------------------------------------------------------------------------

 @file    pwd.c
 @brief   Implementation of the 'pwd' command for HanOS userspace
 @details
 @verbatim

  This file provides the implementation of the 'pwd' command which is used to
  print the current working directory. It includes the main function that calls
  sys_getcwd to retrieve the current directory path and then prints it to the
  standard output. Error handling is included for the getcwd operation.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
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

