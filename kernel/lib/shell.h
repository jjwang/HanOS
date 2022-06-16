/**-----------------------------------------------------------------------------

 @file    shell.h
 @brief   Definition of shell related data structures and functions
 @details
 @verbatim

  - The command and the corresponding processing function.
  - desc[] stores information to be displayed in the help command.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

typedef void (*command_proc_t)(void);

typedef struct {
    char command[128];
    command_proc_t proc;
    char desc[128];
} command_t;


