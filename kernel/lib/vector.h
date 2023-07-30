/**-----------------------------------------------------------------------------

 @file    vector.h
 @brief   vector - fundamental data structure, variable length array
 @details
 @verbatim

  Implementation of variable length array. All the functions are defined in
  macros.

 @endverbatim

 @attention If memories are randomly initialized, random "data" will cause #PF.
 @attention Pls make sure memories are initially set to zero.

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <lib/kmalloc.h>

#define VECTOR_RESIZE_FACTOR        4

#define vec_struct(type)                                            \
    struct {                                                        \
        size_t len;                                                 \
        size_t capacity;                                            \
        type*  data;                                                \
    }

#define vec_extern(type, name)      extern vec_struct(type) name
#define vec_new(type, name)         vec_struct(type) name = {0}
#define vec_new_static(type, name)  static vec_new(type, name)

#define vec_push_back(vec, elem)                                    \
    {                                                               \
        (vec)->len++;                                               \
        if ((vec)->capacity < (vec)->len * sizeof(elem)) {          \
            (vec)->capacity = (vec)->len * sizeof(elem)             \
                              * VECTOR_RESIZE_FACTOR;               \
            (vec)->data = kmrealloc((vec)->data, (vec)->capacity);  \
        }                                                           \
        (vec)->data[(vec)->len - 1] = elem;                         \
    }

#define vec_length(vec)             (vec)->len
#define vec_at(vec, index)          (vec)->data[index]

#define vec_erase(vec, index)                                       \
    {                                                               \
        memcpy(&((vec)->data[index]), &((vec)->data[index + 1]),    \
               sizeof((vec)->data[0]) * (vec)->len - index - 1);    \
        (vec)->len--;                                               \
    }

#define vec_erase_all(vec)                                          \
    {                                                               \
        (vec)->len = 0;                                             \
        (vec)->capacity = 0;                                        \
        if ((vec)->data != NULL) kmfree((vec)->data);               \
        (vec)->data = NULL;                                         \
    }

#define vec_erase_val(vec, val)                                     \
    {                                                               \
        for(size_t __i = 0; __i < (vec)->len; __i++) {              \
            if (vec_at(vec, __i) == (val)) {                        \
                vec_erase(vec, __i);                                \
                break;                                              \
            }                                                       \
        }                                                           \
    }

