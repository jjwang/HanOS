/**-----------------------------------------------------------------------------

 @file    task.c
 @brief   Implementation of task related functions
 @details
 @verbatim

  Create and return task data structure which contains registers and other task
  related information.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <proc/task.h>
#include <proc/sched.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <core/cpu.h>

static task_id_t curr_tid = 0;

task_t* task_make(
    const char *name, void (*entry)(task_id_t), task_priority_t priority,
    task_mode_t mode)
{
    if (curr_tid == TID_MAX) {
        klogw("Could not allocate tid\n");
        return NULL;
    }

    task_t* ntask = kmalloc(sizeof(task_t));
    memset(ntask, 0, sizeof(task_t));

    ntask->tstack_limit = kmalloc(STACK_SIZE);
    ntask->tstack_top = ntask->tstack_limit + STACK_SIZE;

    ntask->kstack_limit = umalloc(STACK_SIZE);
    ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

    task_regs_t* ntask_regs = NULL;
    if (mode == TASK_KERNEL_MODE) {
        ntask_regs = ntask->tstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CODE;
        ntask_regs->ss = DEFAULT_KMODE_DATA;

        ntask_regs->rsp = (uint64_t)ntask->tstack_top;
    } else {
        ntask_regs = ntask->kstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_UMODE_CODE;
        ntask_regs->ss = DEFAULT_UMODE_DATA;

        ntask_regs->rsp = (uint64_t)ntask->kstack_top;
    }

    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t)entry;
    ntask_regs->rdi = curr_tid;

    ntask->tstack_top = ntask_regs;
    ntask->tid = curr_tid;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    /* ntask->addrspace = create_addrspace(); */

    klogi("TASK: Create tid %d in function %s\n", ntask->tid, name);

    curr_tid++;
    return ntask;
}

