#pragma once

#include <stddef.h>
#include <stdint.h>

void* kmalloc(uint64_t size);
void kmfree(void* addr);
void* kmrealloc(void* addr, size_t newsize);
