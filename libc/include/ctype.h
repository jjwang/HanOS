/**-----------------------------------------------------------------------------

 @file    ctype.h
 @brief   Character type and conversion functions for HanOS standard library
 @details
 @verbatim

  This file contains the declarations of functions used for character type
  checking and conversion in the HanOS standard library. It includes functions
  to check if a character is alphanumeric, alphabetic, blank, control, digit,
  graphical, lowercase, printable, punctuation, whitespace, uppercase, or
  hexadecimal digit. Additionally, it provides functions to convert characters
  to lowercase and uppercase.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define KB  ((uint64_t)1024)
#define MB  (((uint64_t)1024 * KB))
#define GB  (((uint64_t)1024 * MB))
#define TB  (((uint64_t)1024 * GB))

int32_t isalnum(int32_t c);
int32_t isalpha(int32_t c);
int32_t isblank(int32_t c);
int32_t iscntrl(int32_t c);
int32_t isdigit(int32_t c);
int32_t isgraph(int32_t c);
int32_t islower(int32_t c);
int32_t isprint(int32_t c);
int32_t ispunct(int32_t c);
int32_t isspace(int32_t c);
int32_t isupper(int32_t c);
int32_t isxdigit(int32_t c);
int32_t tolower(int32_t c);
int32_t toupper(int32_t c);

