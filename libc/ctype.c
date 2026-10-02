/**-----------------------------------------------------------------------------

 @file    ctype.c
 @brief   Implementation of character type and conversion functions
 @details
 @verbatim

  This file contains the implementation of functions for character type checking
  and conversion in the HanOS standard library. The functions include checks for
  alphanumeric characters, alphabetic characters, blank characters, control
  characters, digits, graphical characters, lowercase characters, printable
  characters, punctuation, whitespace characters, uppercase characters, and
  hexadecimal digits. Additionally, it provides functions to convert characters
  to lowercase and uppercase.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <ctype.h>

int32_t isalnum(int32_t c)
{
    return isalpha(c) || isdigit(c);
}

int32_t isalpha(int32_t c)
{
    return (c >= 'A' && c <= 'Z')
        || (c >= 'a' && c <= 'z');
}

int32_t isblank(int32_t c)
{
    return c == ' ' || c == '\t';
}

int32_t iscntrl(int32_t c)
{
    return c >= 0 && c <= 31;
}

int32_t isdigit(int32_t c)
{
    return c >= '0' && c <= '9';
}

int32_t isgraph(int32_t c)
{
    return c >= 33 && c <= 126;
}

int32_t islower(int32_t c)
{
    return c >= 'a' && c <= 'z';
}

int32_t isprint(int32_t c)
{
    return c >= 32 && c <= 126;
}

int32_t ispunct(int32_t c)
{
    return (c >= 33 && c <= 47)
        || (c >= 58 && c <= 64)
        || (c >= 91 && c <= 96)
        || (c >= 123 && c <= 126);
}

int32_t isspace(int32_t c)
{
    return (c == ' ')
        || (c >= 9 && c <= 13);
}

int32_t isupper(int32_t c)
{
    return c >= 'A' && c <= 'Z';
}

int32_t isxdigit(int32_t c)
{
    return isdigit(c)
        || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

int32_t tolower(int32_t c)
{
    if (!isalpha(c) || islower(c)) {
        return c;
    }

    return c + 32;
}

int32_t toupper(int32_t c)
{
    if (!isalpha(c) || isupper(c)) {
        return c;
    }

    return c - 32;
}
