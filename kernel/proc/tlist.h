/**-----------------------------------------------------------------------------

 @file    tlist.h
 @brief   Definition of task list related functions
 @details
 @verbatim

  Push and pop task data structure from task list.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <proc/task.h>

typedef struct {
    task_t* head;
    task_t* tail;
    size_t size;
} task_list_t;

task_t* task_list_pop(task_list_t* tl);
void task_list_push(task_list_t* tl, task_t* t);
