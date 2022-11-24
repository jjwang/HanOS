/**-----------------------------------------------------------------------------

 @file    string.h
 @brief   Definition of string operation related functions
 @details
 @verbatim

  String functions including length, comparison, copy etc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <lib/ctype.h>

typedef enum {
    OCT,
    DEC
} num_sys_t;

int strlen(const char* s);
int strcmp(const char* a, const char* b); 
int strncmp(const char* a, const char* b, size_t len);
int strcpy(char* dest, const char* src);
int strcat(char* dest, const char* src);
char *strchrnul(const char *s, int c);
uint64_t strtol(char* s, num_sys_t type);
