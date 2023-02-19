#pragma once

#include <stddef.h>
#include <stdint.h>

#define LAUNCHER_CLI        true
#define LAUNCHER_GRAPHICS   true

#ifndef ENABLE_BASH
#define DEFAULT_SHELL_APP   "/bin/hansh"
#else
#define DEFAULT_SHELL_APP   "/usr/bin/bash"
#endif

#define DEFAULT_INPUT_SVR   "/server/input"

typedef struct {
    uint64_t screen_hor_size;
    uint64_t screen_ver_size;
    uint64_t prefer_res_x;
    uint64_t prefer_res_y;
} computer_info_t;

