#pragma once

#include <stddef.h>

#define ALLOC_MAX_SIZE     524288

void alloc_init();
void *alloc(size_t s);
void *realloc(void *addr, size_t s);
void free(void *addr);

