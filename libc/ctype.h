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

int isalnum(int c);
int isalpha(int c);
int isblank(int c);
int iscntrl(int c);
int isdigit(int c);
int isgraph(int c);
int islower(int c);
int isprint(int c);
int ispunct(int c);
int isspace(int c);
int isupper(int c);
int isxdigit(int c);
int tolower(int c);
int toupper(int c);

