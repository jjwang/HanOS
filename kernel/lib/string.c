/**-----------------------------------------------------------------------------

 @file    string.c
 @brief   Implementation of string operation related functions
 @details
 @verbatim

  String functions including length, comparison, copy etc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <lib/string.h>
#include <lib/klog.h>

int strlen(const char* s)
{
    int len;

    len = 0;
    while(s[len] != '\0') {
        len++;
    }
    
    return len;   
}


int strcmp(const char* a, const char* b)
{   
    for (size_t i = 0;; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }
}

int strncmp(const char* a, const char* b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }   
    return 0;
}

int strcpy(char* dest, const char* src)
{
    size_t i;
    for (i = 0;; i++) {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    return i;
}

int strcat(char* dest, const char* src)
{
    size_t i, dest_len = strlen(dest);
    for (i = dest_len;; i++) {
        dest[i] = src[i - dest_len];
        if (src[i - dest_len] == '\0')
            break;
    }
    return i;
}

