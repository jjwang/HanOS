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

#include <libc/string.h>

#include <proc/task.h>
#include <proc/sched.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <sys/cpu.h>
#include <sys/hpet.h>
#include <sys/apic.h>

static task_id_t curr_tid = 1;

task_t *task_make(
    const char *name, void (*entry)(task_id_t), task_priority_t priority,
    task_mode_t mode, addrspace_t *pas)
{
    if (curr_tid == TID_MAX) {
        klogw("Could not allocate tid\n");
        return NULL;
    }

    task_t *ntask = kmalloc(sizeof(task_t));
    memset(ntask, 0, sizeof(task_t));

    ntask->tid = curr_tid;

    task_regs_t *ntask_regs = NULL;
    addrspace_t *as = create_addrspace();

    if (mode == TASK_USER_MODE) {
        ntask->kstack_limit = (void*)kmalloc(STACK_SIZE);
        ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

        ntask->ustack_limit = (void*)VIRT_TO_PHYS(kmalloc(STACK_SIZE));
        ntask->ustack_top = ntask->ustack_limit + STACK_SIZE;

        klogi("TASK: %s task id %d (0x%x) kstack 0x%x ustack 0x%x\n",
              name, ntask->tid, ntask, ntask->kstack_top, ntask->ustack_top);

        ntask->tstack_top = ntask->ustack_top;
        ntask->tstack_limit = ntask->ustack_limit;

        /* Notice that the below should be unmapped at the end of this func */
        vmm_map(pas, (uint64_t)ntask->ustack_limit,
                (uint64_t)ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        vmm_map(as, (uint64_t)ntask->ustack_limit,
                (uint64_t)ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        mem_map_t m;

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

    /* If temporarily set to NULL, CR3 switch will be disabled */
    ntask->addrspace = as;

    ntask_regs->rsp = (uint64_t)ntask->tstack_top;
    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t)entry;
    ntask_regs->rdi = curr_tid;

    ntask->mode = mode;
    ntask->tstack_top = ntask_regs;
    ntask->ptid = TID_MAX;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    strcpy(ntask->cwd, "/");
    strncpy(ntask->name, name, sizeof(ntask->name));

    klogi("TASK: Create tid %d with name \"%s\" (task 0x%x)\n",
          ntask->tid, name, ntask);

    curr_tid++;

    if (mode == TASK_USER_MODE) {
        vmm_unmap(pas, (uint64_t)ntask->ustack_limit, NUM_PAGES(STACK_SIZE));
    }

    /* MEMMAP: hpet should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t)hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base), 1,
            VMM_FLAGS_MMIO);

    return ntask;
}

void task_debug(task_t *t, bool force)
{
    klogd("TASK: #%d with PML4 0x%x\n"
          "kstack limit 0x%x, top 0x%x, limit_top 0x%x\n"
          "ustack limit 0x%x, top 0x%x, limit_top 0x%x\n"
          "tstack limit 0x%x, top 0x%x, limit_top 0x%x\n",
          t->tid, t->addrspace != NULL ? t->addrspace->PML4 : NULL,
          t->kstack_limit, t->kstack_top, t->kstack_limit + STACK_SIZE,
          t->ustack_limit, t->ustack_top, t->ustack_limit + STACK_SIZE,
          t->tstack_limit, t->tstack_top, t->tstack_limit + STACK_SIZE);

    if (force || ((uint64_t)t->tstack_top >= (uint64_t)t->kstack_limit
        && (uint64_t)t->tstack_top <= (uint64_t)(t->kstack_limit + STACK_SIZE)))
    {
        task_regs_t *tr = (task_regs_t*)t->tstack_top;
        if (force) tr = (task_regs_t*)PHYS_TO_VIRT(t->tstack_top);
        klogd("Dump registers: \nRIP   : 0x%x\nCS    : 0x%x\nRFLAGS: 0x%x\n"
              "RSP   : 0x%x\nSS    : 0x%x\n"
              "RAX 0x%x  RBX 0x%x  RCX 0x%x  RDX 0x%x\n"
              "RSI 0x%x  RDI 0x%x  RBP 0x%x\n"
              "R8  0x%x  R9  0x%x  R10 0x%x  R11 0x%x\n"
              "R12 0x%x  R13 0x%x  R14 0x%x  R15 0x%x\n",
              tr->rip, tr->cs, tr->rflags, tr->rsp, tr->ss,
              tr->rax, tr->rbx, tr->rcx, tr->rdx, tr->rsi, tr->rdi, tr->rbp,
              tr->r8, tr->r9, tr->r10, tr->r11, tr->r12, tr->r13, tr->r14,
              tr->r15);
    }
}

task_t *task_fork(task_t *tp)
{
    task_debug(tp, false);

    task_t *tc = (task_t*)kmalloc(sizeof(task_t));
    if (tc == NULL) goto norm_exit;

    memcpy(tc, tp, sizeof(task_t));
    memset(&tc->mmap_list, 0, sizeof(tc->mmap_list));

    tc->addrspace = create_addrspace();

    size_t len = vec_length(&(tp->mmap_list));
    klogd("task_fork: totally %d memory blocks\n", len);
    for (size_t i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->mmap_list), i);
        uint64_t ptr = VIRT_TO_PHYS(kmalloc(m.np * PAGE_SIZE));
        memcpy((void*)PHYS_TO_VIRT(ptr), (void*)PHYS_TO_VIRT(m.paddr),
               m.np * PAGE_SIZE);
        if ((uint64_t)tp->ustack_limit == (uint64_t)m.paddr) {
            klogd("task_fork: new user stack 0x%x and map to 0x%x "
                  "with top 0x%x\n", ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        if ((uint64_t)tp->kstack_limit == (uint64_t)m.vaddr) {
            klogd("task_fork: new kern stack 0x%x and map to 0x%x "
                  "with top 0x%x\n", ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        vmm_map(tc->addrspace, m.vaddr, ptr, m.np, m.flags);

        m.paddr = ptr;
        vec_push_back(&tc->mmap_list, m);
    }

    tc->tid = curr_tid;
    tc->ptid = tp->tid;

    tc->kstack_limit = kmalloc(STACK_SIZE);
    memcpy(tc->kstack_limit, tp->kstack_limit, STACK_SIZE);

    uint64_t offset = 0;

    offset = (uint64_t)tc->kstack_top - (uint64_t)tp->kstack_limit;
    tc->kstack_top = (void*)((uint64_t)tc->kstack_limit + offset);

    if ((uint64_t)tc->tstack_top >= (uint64_t)tp->kstack_limit
        && (uint64_t)tc->tstack_top <= (uint64_t)(tp->kstack_limit + STACK_SIZE))
    {
        offset = (uint64_t)tc->tstack_top - (uint64_t)tp->kstack_limit;
        tc->tstack_top = (void*)((uint64_t)tc->kstack_limit + offset);
 
        task_regs_t *tr = (task_regs_t*)tc->tstack_top;

        offset = (uint64_t)tr->rsp - (uint64_t)tp->kstack_limit;
        tr->rsp = (uint64_t)tc->kstack_limit + offset;

        offset = (uint64_t)tr->rbp - (uint64_t)tp->kstack_limit;
        tr->rbp = (uint64_t)tc->kstack_limit + offset;
    }

    task_debug(tc, false);

    /* MEMMAP: hpet should be visible for all kernel tasks */
    vmm_map(tc->addrspace, (uint64_t)hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(tc->addrspace, (uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base), 1,
            VMM_FLAGS_MMIO);

    vec_push_back(&tp->child_list, tc->tid);

    curr_tid++;

norm_exit:
    return tc;
}

