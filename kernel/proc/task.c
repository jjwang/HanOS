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
    ntask->isforked = false;

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

    ht_init(&ntask->openfiles);

    klogi("TASK: Create tid %d with name \"%s\" (task 0x%x)\n",
          ntask->tid, name, ntask);

    curr_tid++;

    if (mode == TASK_USER_MODE) {
        vmm_unmap(pas, (uint64_t)ntask->ustack_limit, NUM_PAGES(STACK_SIZE));
    }

#ifndef ENABLE_MEM_DEBUG
    /* MEMMAP: hpet should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t)hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base), 1,
            VMM_FLAGS_MMIO);
#endif

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
    size_t i;

    task_debug(tp, false);

    task_t *tc = (task_t*)kmalloc(sizeof(task_t));
    if (tc == NULL) goto norm_exit;

    memcpy(tc, tp, sizeof(task_t));
    memset(&tc->mmap_list, 0, sizeof(tc->mmap_list));
    memset(&tc->child_list, 0, sizeof(tc->child_list));

    tc->isforked = true;
    tc->addrspace = create_addrspace();

    size_t len = vec_length(&(tp->mmap_list));
    klogi("task_fork: totally %d memory blocks (parent #%d, child #%d)\n",
          len, tp->tid, curr_tid);
    for (i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->mmap_list), i);
        uint64_t ptr = VIRT_TO_PHYS(kmalloc(m.np * PAGE_SIZE));
        memcpy((void*)PHYS_TO_VIRT(ptr), (void*)PHYS_TO_VIRT(m.paddr),
               m.np * PAGE_SIZE);
        if ((uint64_t)tp->ustack_limit == (uint64_t)m.vaddr) {
            klogi("task_fork: #%d (parent #%d) new user stack 0x%x and "
                  "map to 0x%x with top 0x%x\n",
                  curr_tid, tp->tid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        if ((uint64_t)tp->kstack_limit == (uint64_t)m.vaddr) {
            klogi("task_fork: #%d (parent #%d) new kern stack 0x%x and "
                  "map to 0x%x with top 0x%x\n",
                  curr_tid, tp->tid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
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

    /* Increase refcount of all open files */
    memcpy(&tc->openfiles, &tp->openfiles, sizeof(ht_t));
    for (i = 0; i < HT_ARRAY_SIZE; i++) {
        if (tc->openfiles.array[i].key == -1
            || tc->openfiles.array[i].data == NULL)
        {
            continue;
        }
        vfs_node_desc_t* nd = (vfs_node_desc_t*)kmalloc(sizeof(vfs_node_desc_t));
        memcpy(nd, tc->openfiles.array[i].data, sizeof(vfs_node_desc_t));
        tc->openfiles.array[i].data = nd; 
        nd->inode->refcount++;
        klogd("TASK: copy fd %d from tid %d to tid %d\n",
              tc->openfiles.array[i].key, tp->tid, tc->tid);
    }

    task_debug(tc, false);

    /* MEMMAP: hpet should be visible for all kernel tasks
     *
     * Note that if we open mem debug option, hpet is already visible for
     * all kernel tasks.
     */
#ifndef ENABLE_MEM_DEBUG
    vmm_map(tc->addrspace, (uint64_t)hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(tc->addrspace, (uint64_t)lapic_base, VIRT_TO_PHYS(lapic_base),
            1, VMM_FLAGS_MMIO);
#endif

    klogd("TASK: child tid %d and parent tid %d\n", tc->tid, tp->tid);
    vec_push_back(&tp->child_list, tc->tid);

    curr_tid++;

norm_exit:
    return tc;
}

void task_free(task_t *t)
{
    size_t mmap_num = vec_length(&t->mmap_list);
    for (size_t i = 0; i < mmap_num; i++) {
        mem_map_t m = vec_at(&t->mmap_list, i); 
        vmm_unmap(t->addrspace, m.vaddr, m.np);
        kmfree((void*)PHYS_TO_VIRT(m.paddr));
    }
    vec_erase_all(&t->mmap_list);
    vec_erase_all(&t->child_list);
    vec_erase_all(&t->dup_list);

    klogi("task_idle: dead task tid %d free mmap number %d\n",
          t->tid, mmap_num);

    /* Free memory when creating a new task */
    if (t->mode == TASK_USER_MODE) {
        /* Notes that ustack memory is already free in mmap_list */
    }
    kmfree((void*)t->kstack_limit);

    size_t mem_num = vec_length(&t->addrspace->mem_list);
    for (size_t i = 0; i < mem_num; i++) {
        /*
         * Maybe it was already freed in unmap(), but it is also
         * OK freed here because pmm_free will not crash.
         *
         * Mar 2024 - if it was freed in unmap(), it should not be freed here.
         * The root cause of ELF loading failure is repeatly release of memories
         * in mem_list.
         */
        uint64_t m = vec_at(&t->addrspace->mem_list, i); 
        pmm_free(m, 8, __func__, __LINE__);
    }
    vec_erase_all(&t->addrspace->mem_list);

    kmfree((void*)t->addrspace->PML4);
    kmfree((void*)t->addrspace);

    /*
     * Feb 2024 - An extra task status - TASK_DYING is defined to make sure
     * when we free resources of a dead task, it's all children tasks must be
     * dead.
     */
    klogw("TASK: try to free task %d (forked: %s)\n",
          t->tid, t->isforked ? "true" : "false");
    kmfree(t);
}

