/**-----------------------------------------------------------------------------

 @file    task.h
 @brief   Definition of task related functions
 @details
 @verbatim

  Create and return task data structure which contains registers and other task
  related information.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lib/time.h>
#include <lib/vector.h>
#include <core/smp.h>
#include <fs/vfs.h>

#define KSTACK_SIZE             4096

#define DEFAULT_KMODE_CS        0x08
#define DEFAULT_KMODE_SS        0x10
#define DEFAULT_UMODE_CS        0x1b
#define DEFAULT_UMODE_SS        0x23
#define DEFAULT_RFLAGS          0x0202

#define TID_MAX                 UINT16_MAX

typedef uint16_t task_id_t;
typedef uint8_t task_priority_t;

typedef enum {
    TASK_KERNEL_MODE,
    TASK_USER_MODE
} task_mode_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD
} task_status_t;

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} task_regs_t;

typedef struct task_t {
    void* kstack_top;
    void* kstack_limit;
    void* ustack_top;
    void* ustack_limit;

    task_id_t tid;
    task_priority_t priority;
    uint64_t last_tick;
    uint64_t wakeup_time;
    task_status_t status;
    task_mode_t mode;

    vec_struct(vfs_node_desc_t*) openfiles;

    struct task_t* next;
    struct task_t* prev;
} task_t;

task_t* task_make(void (*entry)(task_id_t), task_priority_t priority, task_mode_t mode);

