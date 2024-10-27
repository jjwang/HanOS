#pragma once

#include <stddef.h>

#define ALLOC_MAX_SIZE     65536

void alloc_init();
void *alloc(uint64_t s);
void *realloc(void *addr, uint64_t s);
void free(void *addr);

