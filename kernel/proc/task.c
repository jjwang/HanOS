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
#include <base/spinlock.h>
#include <sys/cpu.h>
#include <sys/hpet.h>
#include <sys/apic.h>
#include <sys/panic.h>

static task_id_t curr_tid = 1;

task_t *task_make(const char *name, void (*entry)(task_id_t),
                  task_priority_t priority, task_mode_t mode,
                  addrspace_t * pas)
{
    if(curr_tid == TID_MAX) {
        klogw("Could not allocate tid\n");
        return NULL;
    }

    task_t *ntask = kmalloc(sizeof(task_t));
    memset(ntask, 0, sizeof(task_t));

    ntask->tid = curr_tid;
    ntask->isforked = false;

    task_regs_t *ntask_regs = NULL;

    /* All kernel tasks share the same address space */
    addrspace_t *as = NULL;

    if (mode == TASK_USER_MODE) {
        as = create_addrspace();

        ntask->kstack_limit =
            (void *) kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
        ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

        ntask->ustack_limit = (void *)
            VIRT_TO_PHYS(kmalloc_chunk(STACK_SIZE, __func__, __LINE__));
        ntask->ustack_top = ntask->ustack_limit + STACK_SIZE;

        klogi("TASK: %s task id %ld (0x%016lx) kstack 0x%016lx ustack 0x%016lx\n",
              name, ntask->tid, ntask, ntask->kstack_top,
              ntask->ustack_top);

        ntask->tstack_top = ntask->ustack_top;
        ntask->tstack_limit = ntask->ustack_limit;

        /* Notice that the below should be unmapped at the end of this func */
        vmm_map(pas, (uint64_t) ntask->ustack_limit,
                (uint64_t) ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        vmm_map(as, (uint64_t) ntask->ustack_limit,
                (uint64_t) ntask->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        mem_map_t m;

        m.vaddr = (uint64_t) ntask->ustack_limit;
        m.paddr = (uint64_t) ntask->ustack_limit;
        m.np = NUM_PAGES(STACK_SIZE);
        m.flags = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;

        vec_push_back(&ntask->mmap_list, m);

        ntask_regs = ntask->ustack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_UMODE_CODE;
        ntask_regs->ss = DEFAULT_UMODE_DATA;
    } else {
        ntask->kstack_limit =
            kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
        ntask->kstack_top = ntask->kstack_limit + STACK_SIZE;

        ntask->ustack_limit = NULL;
        ntask->ustack_top = NULL;

        klogi("TASK: %s 0x%016lx kstack 0x%016lx ustack 0x%016lx\n",
              name, ntask, ntask->kstack_top, ntask->ustack_top);

        ntask->tstack_top = ntask->kstack_top;
        ntask->tstack_limit = ntask->kstack_limit;

        ntask_regs = ntask->kstack_top - sizeof(task_regs_t);

        ntask_regs->cs = DEFAULT_KMODE_CODE;
        ntask_regs->ss = DEFAULT_KMODE_DATA;
    }

    /* If temporarily set to NULL, CR3 switch will be disabled */
    ntask->addrspace = as;

    ntask_regs->rsp = (uint64_t) ntask->tstack_top;
    ntask_regs->rflags = DEFAULT_RFLAGS;
    ntask_regs->rip = (uint64_t) entry;
    ntask_regs->rdi = curr_tid;

    ntask->mode = mode;
    ntask->tstack_top = ntask_regs;
    ntask->ptid = TID_MAX;
    ntask->priority = priority;
    ntask->last_tick = 0;
    ntask->status = TASK_READY;

    strcpy(ntask->cwd, "/");
    strncpy(ntask->name, name, sizeof(ntask->name));

    ht_init(&ntask->open_files_table, HT_DEFAULT_ARRAY_SIZE);

    klogi("TASK: Create tid %ld with name \"%s\" (task 0x%016lx)\n",
          ntask->tid, name, ntask);

    curr_tid++;

    if (mode == TASK_USER_MODE) {
        vmm_unmap(pas, (uint64_t) ntask->ustack_limit,
                  NUM_PAGES(STACK_SIZE));
    }
#ifndef ENABLE_MEM_DEBUG
    /* MEMMAP: hpet should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t) hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(ntask->addrspace, (uint64_t) lapic_base,
            VIRT_TO_PHYS(lapic_base), 1, VMM_FLAGS_MMIO);
#endif

    return ntask;
}

task_t *task_fork(task_t * tp)
{
    if (tp->mode != TASK_USER_MODE) {
        kpanic("Task: cannot fork kernel task %ld\n", tp->tid);
    }

    task_t *tc = (task_t *) kmalloc(sizeof(task_t));
    if (tc == NULL)
        goto norm_exit;

    memcpy(tc, tp, sizeof(task_t));

    memset(&tc->mmap_list, 0, sizeof(tc->mmap_list));
    memset(&tc->child_list, 0, sizeof(tc->child_list));

    tc->isforked = true;
    tc->addrspace = create_addrspace();

    uint64_t len = vec_length(&(tp->mmap_list));
    klogi("task_fork: totally %ld memory blocks (parent #%ld, child #%ld)\n",
          len, tp->tid, curr_tid);

    uint64_t i;
    for (i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->mmap_list), i);
        uint64_t ptr =
            VIRT_TO_PHYS(kmalloc_chunk
                         (m.np * PAGE_SIZE, __func__, __LINE__));
        memcpy((void *) PHYS_TO_VIRT(ptr), (void *) PHYS_TO_VIRT(m.paddr),
               m.np * PAGE_SIZE);
        if ((uint64_t) tp->ustack_limit == (uint64_t) m.vaddr) {
            klogi("task_fork: #%ld (parent #%ld) new user stack 0x%016lx and "
                  "map to 0x%016lx with top 0x%016lx\n",
                  curr_tid, tp->tid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        if ((uint64_t) tp->kstack_limit == (uint64_t) m.vaddr) {
            klogi("task_fork: #%ld (parent #%ld) new kern stack 0x%016lx and "
                  "map to 0x%016lx with top 0x%016lx\n",
                  curr_tid, tp->tid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        vmm_map(tc->addrspace, m.vaddr, ptr, m.np, m.flags);

        m.paddr = ptr;
        vec_push_back(&tc->mmap_list, m);
    }

    tc->tid = curr_tid;
    tc->ptid = tp->tid;

    tc->kstack_limit = kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
    memcpy(tc->kstack_limit, tp->kstack_limit, STACK_SIZE);

    uint64_t offset = 0;

    offset = (uint64_t) tc->kstack_top - (uint64_t) tp->kstack_limit;
    tc->kstack_top = (void *) ((uint64_t) tc->kstack_limit + offset);

    if ((uint64_t) tc->tstack_top >= (uint64_t) tp->kstack_limit
        && (uint64_t) tc->tstack_top <=
        (uint64_t) (tp->kstack_limit + STACK_SIZE)) {
        offset = (uint64_t) tc->tstack_top - (uint64_t) tp->kstack_limit;
        tc->tstack_top = (void *) ((uint64_t) tc->kstack_limit + offset);

        task_regs_t *tr = (task_regs_t *) tc->tstack_top;

        offset = (uint64_t) tr->rsp - (uint64_t) tp->kstack_limit;
        tr->rsp = (uint64_t) tc->kstack_limit + offset;

        offset = (uint64_t) tr->rbp - (uint64_t) tp->kstack_limit;
        tr->rbp = (uint64_t) tc->kstack_limit + offset;
    }

    /* Increase refcount of all open files */
    ht_init(&tc->open_files_table, tp->open_files_table.size);
    for (i = 0; i < tp->open_files_table.size; i++) {
        if (tp->open_files_table.array[i].key == -1
            || tp->open_files_table.array[i].data == NULL) {
            continue;
        }
        vfs_node_desc_t *fd =
            (vfs_node_desc_t *) kmalloc(sizeof(vfs_node_desc_t));
        memcpy(fd, tp->open_files_table.array[i].data,
               sizeof(vfs_node_desc_t));
        tc->open_files_table.array[i].key =
            tp->open_files_table.array[i].key;
        tc->open_files_table.array[i].data = fd;
        fd->inode->refcount++;
        if (fd->mode == VFS_MODE_READ) {
            fd->inode->readcount++;
        } else if (fd->mode == VFS_MODE_WRITE) {
            fd->inode->writecount++;
        } else {
            fd->inode->readcount++;
            fd->inode->writecount++;
        }
        klogd("TASK: copy fd %ld from tid %ld to tid %ld\n",
              tc->open_files_table.array[i].key, tp->tid, tc->tid);
    }

    /* MEMMAP: hpet should be visible for all kernel tasks
     *
     * Note that if we open mem debug option, hpet is already visible for
     * all kernel tasks.
     */
#if !ENABLE_MEM_DEBUG
    vmm_map(tc->addrspace, (uint64_t) hpet, VIRT_TO_PHYS(hpet),
            1, VMM_FLAGS_MMIO);

    /* MEMMAP: lapic_base should be visible for all kernel tasks */
    vmm_map(tc->addrspace, (uint64_t) lapic_base, VIRT_TO_PHYS(lapic_base),
            1, VMM_FLAGS_MMIO);
#endif

    klogd("TASK: child tid %ld and parent tid %ld\n", tc->tid, tp->tid);
    vec_push_back(&tp->child_list, tc->tid);

    curr_tid++;

  norm_exit:
    return tc;
}

void task_free(task_t * t)
{
    if (t->mode != TASK_USER_MODE) {
        kpanic("Task: cannot free kernel task %ld\n", t->tid);
    }

    uint64_t mmap_num = vec_length(&t->mmap_list);
    for (uint64_t i = 0; i < mmap_num; i++) {
        mem_map_t m = vec_at(&t->mmap_list, i);
        vmm_unmap(t->addrspace, m.vaddr, m.np);
        kmfree_chunk((void *) PHYS_TO_VIRT(m.paddr), __func__, __LINE__);
    }
    vec_erase_all(&t->mmap_list);
    vec_erase_all(&t->child_list);
    vec_erase_all(&t->dup_list);

    kmfree_chunk((void *) t->kstack_limit, __func__, __LINE__);

    uint64_t mem_num = vec_length(&t->addrspace->mem_list);
    for (uint64_t i = 0; i < mem_num; i++) {
        /*
         * Maybe it was already freed in unmap(), but it is also
         * harmless for calling pmm_free() in which it will check
         * if the referenced physical page is valid and then
         * do free. VMM_UNMAP() invokes pmm_free() for us, but it
         * will not free the records represented by uint64_t type
         * in mem_list.
         */
        uint64_t m = vec_at(&t->addrspace->mem_list, i);
        pmm_free(m, 8, __func__, __LINE__);
    }
    vec_erase_all(&t->addrspace->mem_list);

    kmfree_chunk((void *) t->addrspace->PML4, __func__, __LINE__);
    kmfree((void *) t->addrspace);

    kmfree(t);
}
