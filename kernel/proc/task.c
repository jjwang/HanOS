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
#include <sys/cpu.h>

static task_id_t curr_tid = 1;

task_t *task_make(
    const char *name, void (*entry)(task_id_t), task_priority_t priority,
    task_mode_t mode)
{
    if (curr_tid == TID_MAX) {
        klogw("Could not allocate tid\n");
        return NULL;
    }

    task_t *ntask = kmalloc(sizeof(task_t));
    memset(ntask, 0, sizeof(task_t));

    ntask->kstack_limit = kmalloc(STACK_SIZE);
    ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

    ntask->ustack_limit = kmalloc(STACK_SIZE);
    ntask->ustack_top = ntask->ustack_limit + STACK_SIZE;

    klogi("TASK: %s 0x%11x kstack 0x%11x ustack 0x%11x\n",
          name, ntask, ntask->kstack_top, ntask->ustack_top);

    task_regs_t *ntask_regs = NULL;
    if (mode == TASK_KERNEL_MODE) {
        ntask_regs = ntask->kstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CODE;
        ntask_regs->ss = DEFAULT_KMODE_DATA;

        ntask->tstack_top = ntask->kstack_top;
        ntask->tstack_limit = ntask->kstack_limit;

        ntask->addrspace = NULL;
    } else {
        ntask_regs = ntask->ustack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_UMODE_CODE;
        ntask_regs->ss = DEFAULT_UMODE_DATA;

        ntask->tstack_top = ntask->ustack_top;
        ntask->tstack_limit = ntask->ustack_limit;

        addrspace_t *as = create_addrspace();
        vmm_map(as, (uint64_t)ntask->tstack_limit,
                VIRT_TO_PHYS((uint64_t)ntask->tstack_limit),
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, false);
        klogd("TASK: map %d bytes memory 0x%x to 0x%x\n",
                STACK_SIZE, VIRT_TO_PHYS((uint64_t)ntask->tstack_limit),
                ntask->tstack_limit);

        /* Temporarily set to NULL to disable CR3 switch */
        ntask->addrspace = as;
    }

    ntask_regs->rsp = (uint64_t)ntask->tstack_top;
    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t)entry;
    ntask_regs->rdi = curr_tid;

    ntask->tstack_top = ntask_regs;
    ntask->tid = curr_tid;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    klogi("TASK: Create tid %d in function %s (task 0x%x)\n",
          ntask->tid, name, ntask);

    curr_tid++;
    return ntask;
}

