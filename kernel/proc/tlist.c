/**-----------------------------------------------------------------------------

 @file    tlist.c
 @brief   Implementation of task list related functions
 @details
 @verbatim

  Push and pop task data structure from task list.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022

 **-----------------------------------------------------------------------------
 */
#include <proc/tlist.h>

void task_list_push(task_list_t* tl, task_t* t)
{
    if (tl->head == NULL) {
        tl->head = t;
        tl->tail = t;
        t->prev = NULL;
        t->next = NULL;
        return;
    } else {
        if (tl->head == tl->tail) {
            tl->head->next = t;
            t->prev = tl->head;
            tl->tail = t;
            t->next = NULL;
        } else {
            tl->tail->next = t;
            t->prev = tl->tail;
            tl->tail = t;
            t->next = NULL;
        }
    }
}

task_t* task_list_pop(task_list_t* tl)
{
    task_t* r = NULL;

    if (tl->head == NULL) {
        return NULL;
    } else {
        if (tl->head == tl->tail) {
            r = tl->head;
            tl->head = NULL;
            tl->tail = NULL;
        } else {
            r = tl->head;
            tl->head = tl->head->next;
            tl->head->prev = NULL;
        }
        return r;
    }
}

