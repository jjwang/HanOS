#pragma once

#include <stddef.h>
#include <proc/task.h>

typedef struct task_list {
    task_t* head;
    task_t* tail;
} task_list_t;

task_t* task_list_pop(task_list_t* tl);
void task_list_push(task_list_t* tl, task_t* t);

