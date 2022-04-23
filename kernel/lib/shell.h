#pragma once

typedef void (*command_proc_t)(void);

typedef struct {
    char command[128];
    command_proc_t proc;
    char desc[128];
} command_t;


