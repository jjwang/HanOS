/**-----------------------------------------------------------------------------

 @file    string.c
 @brief   Implementation of string operation related functions
 @details
 @verbatim

  String functions including length, comparison, copy etc.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

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
