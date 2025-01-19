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

void* memcpy(void *dest, const void *src, uint64_t len)
{
    /* Q = 8 bytes at a time
     * D = 4 bytes at a time
     * W = 2 bytes at a time
     * B = 1 byte  at a time
     */
    if (len % 8 == 0) {
        asm volatile("mov %[len], %%rcx;"
                     "mov %[src], %%rsi;"
                     "mov %[dest], %%rdi;"
                     "rep movsq;"
                     :   
                     : [len] "g"(len / 8), [src] "g"(src), [dest] "g"(dest)
                     : "memory", "rcx", "rsi", "rdi");
    } else if (len % 4 == 0) {
        asm volatile("mov %[len], %%rcx;"
                     "mov %[src], %%rsi;"
                     "mov %[dest], %%rdi;"
                     "rep movsd;"
                     :   
                     : [len] "g"(len / 4), [src] "g"(src), [dest] "g"(dest)
                     : "memory", "rcx", "rsi", "rdi");
    } else if (len % 2 == 0) {
        asm volatile("mov %[len], %%rcx;"
                     "mov %[src], %%rsi;"
                     "mov %[dest], %%rdi;"
                     "rep movsw;"
                     :   
                     : [len] "g"(len / 2), [src] "g"(src), [dest] "g"(dest)
                     : "memory", "rcx", "rsi", "rdi");
    } else {
        asm volatile("mov %[len], %%rcx;"
                     "mov %[src], %%rsi;"
                     "mov %[dest], %%rdi;"
                     "rep movsb;"
                     :   
                     : [len] "g"(len), [src] "g"(src), [dest] "g"(dest)
                     : "memory", "rcx", "rsi", "rdi");
    }

    return dest;
}

void memset(void *addr, uint8_t val, uint64_t len)
{
    uint8_t *a = (uint8_t*)addr;
    for (uint64_t i = 0; i < len; i++)
        a[i] = val;
}

bool memcmp(const void *s1, const void *s2, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        uint8_t a = ((uint8_t*)s1)[i];
        uint8_t b = ((uint8_t*)s2)[i];

        if (a != b)
            return false;
    }   
    return true;
}

int64_t strlen(const char *s)
{
    int64_t len;

    len = 0;
    while(s[len] != '\0') {
        len++;
    }
    
    return len;   
}


int64_t strcmp(const char *a, const char *b)
{   
    for (uint64_t i = 0;; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }
}

int64_t strncmp(const char *a, const char *b, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++) {
        if (a[i] != b[i] || a[i] == '\0' || b[i] == '\0')
            return a[i] - b[i];
    }   
    return 0;
}

char *strcpy(char *__restrict dest, const char *src)
{
    uint64_t i;
    for (i = 0;; i++) {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    return dest + i;
}

char *strncpy(char *__restrict dest, const char *src, uint64_t len)
{
    uint64_t i;
    for (i = 0; i < len; i++) {
        dest[i] = src[i];
        if (src[i] == '\0')
            break;
    }
    return dest + i;
}

int64_t strcat(char *dest, const char *src)
{
    uint64_t i, dest_len = strlen(dest);
    for (i = dest_len;; i++) {
        dest[i] = src[i - dest_len];
        if (src[i - dest_len] == '\0')
            break;
    }
    return i;
}

int64_t strncat(char *dest, const char *src, uint64_t dest_size)
{
    uint64_t i, dest_len = strlen(dest);
    for (i = dest_len; ; i++) {
        dest[i] = src[i - dest_len];
        if (src[i - dest_len] == '\0')
            break;
        if (i >= dest_size - 1)
            break;
    }
    dest[i] = '\0';
    return i;
}

uint64_t strtol(char *s, num_sys_t type)
{
    uint64_t len = strlen(s);
    uint64_t val = 0;
    uint8_t max_single_num = ((type == OCT) ? 7 : 9);

    for (uint64_t i = 0; i < len; i++) {
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
    uint64_t len = strlen(s);
    for (uint64_t i = 0; i < len; i++) {
        if (s[i] >= 'A' && s[i] <= 'Z') {
            s[i] = s[i] - 'A' + 'a';
        }
    }
    return s;
}

char *strupr(char *s)
{
    uint64_t len = strlen(s);
    for (uint64_t i = 0; i < len; i++) {
        if (s[i] >= 'a' && s[i] <= 'z') {
            s[i] = s[i] - 'a' + 'A';
        }   
    }
    return s;
}
