/**-----------------------------------------------------------------------------

 @file    string.h
 @brief   Definition of string operation related functions
 @details
 @verbatim

  String functions including copying, concatenation, comparison, search etc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <libc/ctype.h>

typedef enum {
    OCT,
    DEC
} num_sys_t;

bool memcmp(const void *s1, const void *s2, size_t len);
void memset(void *addr, uint8_t val, size_t len);
void* memcpy(void *dest, const void *src, size_t len);

/* Copying functions */
char *strcpy(char *__restrict dest, const char *src);
char *strncpy(char *__restrict dest, const char *src, size_t len);

int strlen(const char *s);
int strcmp(const char *a, const char *b); 
int strncmp(const char *a, const char *b, size_t len);
int strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
uint64_t strtol(char* s, num_sys_t type);
char *strlwr(char *s);
char *strupr(char *s);
