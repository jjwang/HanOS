#pragma once

#include <stdint.h>

void gpu_log(const char *fmt, ...);
void pit_wait(uint32_t ms);

#define klogi(...) gpu_log(__VA_ARGS__)
#define klogd(...) gpu_log(__VA_ARGS__)
#define klogw(...) gpu_log(__VA_ARGS__)
#define kloge(...) gpu_log(__VA_ARGS__)
