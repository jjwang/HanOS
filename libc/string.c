/**-----------------------------------------------------------------------------

 @file    string.c
 @brief   Implementation of various string manipulation functions.
 @details
 @verbatim

  String functions including length, comparison, copy etc.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

void* memcpy(void *dest, const void *src, size_t len)
{
    asm volatile("mov %[len], %%rcx;"
                 "mov %[src], %%rsi;"
                 "mov %[dest], %%rdi;"
                 "rep movsb;"
                 :   
                 : [len] "g"(len), [src] "g"(src), [dest] "g"(dest)
                 : "memory", "rcx", "rsi", "rdi");

    return dest;
}

void memset(void *addr, uint8_t val, size_t len)
{
    uint8_t *a = (uint8_t*)addr;
    for (uint64_t i = 0; i < len; i++)
        a[i] = val;
}

bool memcmp(const void *s1, const void *s2, size_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        uint8_t a = ((uint8_t*)s1)[i];
        uint8_t b = ((uint8_t*)s2)[i];

        if (a != b)
            return false;
    }   
    return true;
}

int strlen(const char *s)
{
    int len;

    len = 0;
    while(s[len] != '\0') {
        len++;
    }
    
    return len;   
}


int strcmp(const char *a, const char *b)
{   
    for (size_t i = 0;; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }
}

int strncmp(const char *a, const char *b, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }   
    return 0;
}

char *strcpy(char *__restrict dest, const char *src)
{
    size_t i;
    for (i = 0;; i++) {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    return dest + i;
}

char *strncpy(char *__restrict dest, const char *src, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++) {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    return dest + i;
}

int strcat(char *dest, const char *src)
{
    size_t i, dest_len = strlen(dest);
    for (i = dest_len;; i++) {
        dest[i] = src[i - dest_len];
        if (src[i - dest_len] == '\0')
            break;
    }
    return i;
}

uint64_t strtol(char *s, num_sys_t type)
{
    size_t len = strlen(s);
    uint64_t val = 0;
    uint8_t max_single_num = ((type == OCT) ? 7 : 9);

    for (size_t i = 0; i < len; i++) {
        if (s[i] >= '0' && s[i] <= ('0' + max_single_num)) {
            val = val * (max_single_num + 1) + s[i] - '0';
        }
    }

    return val;
}

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == c) {
            return (char*)s;
        }
        s++;
    }

    return (char *)NULL;
}

char *strlwr(char *s)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            s[i] = s[i] - 'A' + 'a';
        }
    }
    return s;
}

char *strupr(char *s)
{
    size_t len = strlen(s);
    for (size_t i = 0; i < len; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] = s[i] - 'a' + 'A';
        }   
    }
    return s;
}
