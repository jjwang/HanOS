/**-----------------------------------------------------------------------------

 @file    task.c
 @brief   Implementation of task related functions
 @details
 @verbatim

  New and fork task data structure which contains registers and other task
  related information. The key of fork operation is to make sure there is an
  entirely same stack and memory copy in the different virtual memory space
  of parent and child tasks.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>

#include <proc/task.h>
#include <proc/sched.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/string.h>
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

    task_regs_t *ntask_regs = NULL;
    addrspace_t *as = create_addrspace();

    if (mode == TASK_USER_MODE) {
        ntask->kstack_limit = (void*)kmalloc(STACK_SIZE);
        ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

        ntask->ustack_limit = (void*)VIRT_TO_PHYS(kmalloc(STACK_SIZE));
        ntask->ustack_top = ntask->ustack_limit + STACK_SIZE;

        klogi("TASK: %s 0x%x kstack 0x%x ustack 0x%x\n",
              name, ntask, ntask->kstack_top, ntask->ustack_top);

        ntask->tstack_top = ntask->ustack_top;
        ntask->tstack_limit = ntask->ustack_limit;

        vmm_map(NULL, (uint64_t)ntask->ustack_limit,
                (uint64_t)ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, false);

        mem_map_t m;

        /* 
         * TODO: We need to know where we will use kernel stack except syscall
         * in kernel operations. This stack should not be placed in higher half
         * memory region.
         */
        vmm_map(as, (uint64_t)ntask->kstack_limit,
                (uint64_t)VIRT_TO_PHYS(ntask->kstack_limit),
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, false);

        m.vaddr = (uint64_t)ntask->kstack_limit;
        m.paddr = (uint64_t)VIRT_TO_PHYS(ntask->kstack_limit);
        m.np = NUM_PAGES(STACK_SIZE);
        m.flags = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;

        vec_push_back(&ntask->mmap_list, m);

        vmm_map(as, (uint64_t)ntask->ustack_limit,
                (uint64_t)ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, false);

        m.vaddr = (uint64_t)ntask->ustack_limit;
        m.paddr = (uint64_t)ntask->ustack_limit;
        m.np = NUM_PAGES(STACK_SIZE);
        m.flags = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE; 

        vec_push_back(&ntask->mmap_list, m);

        ntask_regs = ntask->ustack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_UMODE_CODE;
        ntask_regs->ss = DEFAULT_UMODE_DATA;
    } else {
        ntask->kstack_limit = kmalloc(STACK_SIZE);
        ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

        ntask->ustack_limit = NULL;
        ntask->ustack_top = NULL;

        klogi("TASK: %s 0x%x kstack 0x%x ustack 0x%x\n",
              name, ntask, ntask->kstack_top, ntask->ustack_top);

        ntask->tstack_top = ntask->kstack_top;
        ntask->tstack_limit = ntask->kstack_limit;

        ntask_regs = ntask->kstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CODE;
        ntask_regs->ss = DEFAULT_KMODE_DATA;
    }

    klogd("TASK: map %d bytes memory 0x%x to 0x%x\n",
          STACK_SIZE, VIRT_TO_PHYS((uint64_t)ntask->tstack_limit),
          ntask->tstack_limit);

    /* If temporarily set to NULL, CR3 switch will be disabled */
    ntask->addrspace = as;

    ntask_regs->rsp = (uint64_t)ntask->tstack_top;
    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t)entry;
    ntask_regs->rdi = curr_tid;

    ntask->mode = mode;
    ntask->tstack_top = ntask_regs;
    ntask->tid = curr_tid;
    ntask->ptid = TID_MAX;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    strcpy(ntask->cwd, "/");

    klogi("TASK: Create tid %d in function %s (task 0x%x)\n",
          ntask->tid, name, ntask);

    curr_tid++;

    if (mode == TASK_USER_MODE) {
        vmm_unmap(NULL, (uint64_t)ntask->ustack_limit, NUM_PAGES(STACK_SIZE),
                  false);
    }

    return ntask;
}

task_t *task_fork(task_t *tp)
{
    task_t *tc = (task_t*)kmalloc(sizeof(task_t));
    if (tc == NULL) goto norm_exit;

    memcpy(tc, tp, sizeof(task_t));

    tc->addrspace = create_addrspace();

    size_t len = vec_length(&(tp->mmap_list));
    for (size_t i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->mmap_list), i);
        uint64_t ptr = VIRT_TO_PHYS(kmalloc(m.np * PAGE_SIZE));
        memcpy((void*)PHYS_TO_VIRT(ptr), (void*)PHYS_TO_VIRT(m.paddr),
               m.np * PAGE_SIZE);
        if ((uint64_t)tp->ustack_limit == (uint64_t)m.paddr) {
            klogd("task_fork: new user stack 0x%x and map to 0x%x\n",
                  ptr, m.vaddr);
        }
        vmm_map(tc->addrspace, m.vaddr, ptr, m.np, m.flags, false);
    }

    tc->tid = curr_tid;
    tc->ptid = tp->tid;

    curr_tid++;

norm_exit:
    return tc;
}

