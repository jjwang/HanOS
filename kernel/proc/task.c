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

task_t* task_make(void (*entry)(task_id_t), task_priority_t priority, task_mode_t mode)
{
    if (curr_tid == TID_MAX) {
        klogw("Could not allocate tid\n");
        return NULL;
    }

    task_t* ntask = kmalloc(sizeof(task_t));
    memset(ntask, 0, sizeof(task_t));

    ntask->kstack_limit = kmalloc(KSTACK_SIZE);
    ntask->kstack_top = ntask->kstack_limit + KSTACK_SIZE;

    ntask->ustack_limit = umalloc(KSTACK_SIZE);
    ntask->ustack_top = ntask->ustack_limit + KSTACK_SIZE;

    task_regs_t* ntask_regs = NULL;
    if (mode == TASK_KERNEL_MODE) {
        ntask_regs = ntask->kstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CS;
        ntask_regs->ss = DEFAULT_KMODE_SS;

        ntask_regs->rsp = (uint64_t)ntask->kstack_top;
    } else {
        ntask_regs = ntask->ustack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CS;
        ntask_regs->ss = DEFAULT_KMODE_SS;

        ntask_regs->rsp = (uint64_t)ntask->ustack_top;
    }

    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t)entry;
    ntask_regs->rdi = curr_tid;

    ntask->kstack_top = ntask_regs;
    ntask->tid = curr_tid;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    curr_tid++;
    return ntask;
}

