#pragma once

#include <stddef.h>
#include <stdint.h>

#define LAUNCHER_CLI        true
#define DEFAULT_SHELL_APP   "/bin/sh"

#define DEFAULT_DRIVER_TTY  "/driver/tty"

typedef struct {
    uint64_t screen_hor_size;
    uint64_t screen_ver_size;
    uint64_t prefer_res_x;
    uint64_t prefer_res_y;
} computer_info_t;

