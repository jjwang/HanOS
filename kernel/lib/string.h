/**-----------------------------------------------------------------------------

 @file    string.h
 @brief   Definition of string operation related functions
 @details
 @verbatim

  String functions including length, comparison, copy etc.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>

int strlen(const char* s);
int strcmp(const char* a, const char* b); 
int strncmp(const char* a, const char* b, size_t len);
int strcpy(char* dest, const char* src);
int strcat(char* dest, const char* src);

