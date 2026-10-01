/**-----------------------------------------------------------------------------

 @file    process.c
 @brief   Implementation of process related functions
 @details
 @verbatim

  New and fork process data structure which contains registers and other process
  related information. The key of fork operation is to make sure there is an
  entirely same stack and memory copy in the different virtual memory space
  of parent and child processes.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>

#include <string.h>

#include <proc/process.h>
#include <proc/sched.h>
#include <fs/vfs.h>
#include <router/router.h>
#include <ipc/ipc.h>
#include <protocol.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/process_srv.h>
#include <lib/spinlock.h>
#include <arch/x64/cpu.h>
#include <arch/x64/hpet.h>
#include <arch/x64/apic.h>
#include <arch/x64/panic.h>

static pid_t curr_pid = 1;

/* Global process table: an intrusive chained hash of every live process keyed by
 * pid. It turns pid lookups that used to scan every run queue into a single
 * bucket walk. */
#define PROCESS_TABLE_BUCKETS  256

static process_t *process_table[PROCESS_TABLE_BUCKETS] = { 0 };
static spinlock_t process_table_lock = { 0 };

static uint64_t process_table_bucket(pid_t pid)
{
    return pid % PROCESS_TABLE_BUCKETS;
}

static void process_table_add(process_t * t)
{
    spinlock_acquire(&process_table_lock);

    uint64_t b = process_table_bucket(t->pid);
    t->table_next = process_table[b];
    process_table[b] = t;

    spinlock_release(&process_table_lock);
}

static void process_table_remove(process_t * t)
{
    spinlock_acquire(&process_table_lock);

    uint64_t b = process_table_bucket(t->pid);
    process_t **pp = &process_table[b];
    while (*pp != NULL) {
        if (*pp == t) {
            *pp = t->table_next;
            break;
        }
        pp = &(*pp)->table_next;
    }

    spinlock_release(&process_table_lock);
}

process_t *process_lookup(pid_t pid)
{
    spinlock_acquire(&process_table_lock);

    process_t *t = process_table[process_table_bucket(pid)];
    while (t != NULL && t->pid != pid)
        t = t->table_next;

    spinlock_release(&process_table_lock);
    return t;
}

process_t *process_make(const char *name, void (*entry)(pid_t),
                  process_priority_t priority, process_mode_t mode,
                  addrspace_t * pas)
{
    pid_t new_pid = __atomic_fetch_add(&curr_pid, 1, __ATOMIC_RELAXED);
    if (new_pid >= PID_MAX) {
        klogw("Could not allocate pid\n");
        return NULL;
    }

    process_t *nproc = kmalloc(sizeof(process_t));
    memset(nproc, 0, sizeof(process_t));

    nproc->pid = new_pid;
    nproc->forked = false;

    process_regs_t *nproc_regs = NULL;

    /* All kernel processes share the same address space */
    addrspace_t *as = NULL;

    if (mode == PROC_USER_MODE) {
        as = create_addrspace();

        nproc->kstack_limit =
            (void *) kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
        nproc->kstack_top = nproc->kstack_limit + STACK_SIZE;

        nproc->ustack_limit = (void *)
            VIRT_TO_PHYS(kmalloc_chunk(STACK_SIZE, __func__, __LINE__));
        nproc->ustack_top = nproc->ustack_limit + STACK_SIZE;

        klogi("PROC: %s process id %ld (0x%016lx) kstack 0x%016lx ustack 0x%016lx\n",
              name, nproc->pid, nproc, nproc->kstack_top,
              nproc->ustack_top);

        nproc->context = nproc->ustack_top;

        /* Notice that the below should be unmapped at the end of this func */
        vmm_map(pas, (uint64_t) nproc->ustack_limit,
                (uint64_t) nproc->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        vmm_map(as, (uint64_t) nproc->ustack_limit,
                (uint64_t) nproc->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        mem_map_t m;

        m.vaddr = (uint64_t) nproc->ustack_limit;
        m.paddr = (uint64_t) nproc->ustack_limit;
        m.np = NUM_PAGES(STACK_SIZE);
        m.flags = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;

        vec_push_back(&nproc->mmap_list, m);

        nproc_regs = nproc->ustack_top - sizeof(process_regs_t);

        nproc_regs->cs = DEFAULT_UMODE_CODE;
        nproc_regs->ss = DEFAULT_UMODE_DATA;
    } else {
        nproc->kstack_limit =
            kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
        nproc->kstack_top = nproc->kstack_limit + STACK_SIZE;

        nproc->ustack_limit = NULL;
        nproc->ustack_top = NULL;

        klogi("PROC: %s 0x%016lx kstack 0x%016lx ustack 0x%016lx\n",
              name, nproc, nproc->kstack_top, nproc->ustack_top);

        nproc->context = nproc->kstack_top;

        nproc_regs = nproc->kstack_top - sizeof(process_regs_t);

        nproc_regs->cs = DEFAULT_KMODE_CODE;
        nproc_regs->ss = DEFAULT_KMODE_DATA;
    }

    /* If temporarily set to NULL, CR3 switch will be disabled */
    nproc->addrspace = as;

    nproc_regs->rsp = (uint64_t) nproc->context;
    nproc_regs->rflags = DEFAULT_RFLAGS;
    nproc_regs->rip = (uint64_t) entry;
    nproc_regs->rdi = new_pid;

    nproc->mode = mode;
    nproc->context = nproc_regs;
    nproc->ppid = PID_MAX;
    nproc->priority = priority;
    nproc->last_tick = 0;
    nproc->status = PROC_READY;

    strcpy(nproc->cwd, "/");
    strncpy(nproc->name, name, sizeof(nproc->name));

    handle_table_init(&nproc->handles);
    klogi("PROC: Create pid %ld with name \"%s\" (process 0x%016lx)\n",
          nproc->pid, name, nproc);

    if (mode == PROC_USER_MODE) {
        vmm_unmap(pas, (uint64_t) nproc->ustack_limit,
                  NUM_PAGES(STACK_SIZE));
    }

    /* hpet and lapic_base live in the kernel half, which create_addrspace()
     * shares with every process. */

    process_table_add(nproc);

    return nproc;
}

process_t *process_fork(process_t * tp)
{
    if (tp->mode != PROC_USER_MODE) {
        kpanic("Process: cannot fork kernel process %ld\n", tp->pid);
    }

    process_t *tc = (process_t *) kmalloc(sizeof(process_t));
    if (tc == NULL)
        goto norm_exit;

    memcpy(tc, tp, sizeof(process_t));

    memset(&tc->mmap_list, 0, sizeof(tc->mmap_list));
    memset(&tc->child_list, 0, sizeof(tc->child_list));
    handle_table_init(&tc->handles);

    pid_t new_pid = __atomic_fetch_add(&curr_pid, 1, __ATOMIC_RELAXED);
    spinlock_init(&tc->child_lock);

    tc->forked = true;
    tc->addrspace = create_addrspace();

    uint64_t len = vec_length(&(tp->mmap_list));
    klogi("process_fork: totally %ld memory blocks (parent #%ld, child #%ld)\n",
          len, tp->pid, new_pid);

    uint64_t i;
    for (i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->mmap_list), i);
        uint64_t ptr =
            VIRT_TO_PHYS(kmalloc_chunk
                         (m.np * PAGE_SIZE, __func__, __LINE__));
        memcpy((void *) PHYS_TO_VIRT(ptr), (void *) PHYS_TO_VIRT(m.paddr),
               m.np * PAGE_SIZE);
        if ((uint64_t) tp->ustack_limit == (uint64_t) m.vaddr) {
            klogi("process_fork: #%ld (parent #%ld) new user stack 0x%016lx and "
                  "map to 0x%016lx with top 0x%016lx\n",
                  new_pid, tp->pid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        if ((uint64_t) tp->kstack_limit == (uint64_t) m.vaddr) {
            klogi("process_fork: #%ld (parent #%ld) new kern stack 0x%016lx and "
                  "map to 0x%016lx with top 0x%016lx\n",
                  new_pid, tp->pid, ptr, m.vaddr, m.vaddr + STACK_SIZE);
        }
        vmm_map(tc->addrspace, m.vaddr, ptr, m.np, m.flags);

        m.paddr = ptr;
        vec_push_back(&tc->mmap_list, m);
    }

    tc->pid = new_pid;
    tc->ppid = tp->pid;
    tc->status = PROC_READY;
    process_table_add(tc);

    tc->kstack_limit = kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
    memcpy(tc->kstack_limit, tp->kstack_limit, STACK_SIZE);

    uint64_t offset = 0;

    offset = (uint64_t) tc->kstack_top - (uint64_t) tp->kstack_limit;
    tc->kstack_top = (void *) ((uint64_t) tc->kstack_limit + offset);

    if ((uint64_t) tc->context >= (uint64_t) tp->kstack_limit
        && (uint64_t) tc->context <=
        (uint64_t) (tp->kstack_limit + STACK_SIZE)) {
        offset = (uint64_t) tc->context - (uint64_t) tp->kstack_limit;
        tc->context = (void *) ((uint64_t) tc->kstack_limit + offset);

        process_regs_t *tr = (process_regs_t *) tc->context;

        offset = (uint64_t) tr->rsp - (uint64_t) tp->kstack_limit;
        tr->rsp = (uint64_t) tc->kstack_limit + offset;

        offset = (uint64_t) tr->rbp - (uint64_t) tp->kstack_limit;
        tr->rbp = (uint64_t) tc->kstack_limit + offset;
    }

    /* The process server owns the fd table; it clones the parent's descriptors
     * for the child and takes a reference on each server-side description. */
    process_fd_fork((int) tp->pid, (int) tc->pid);

    /* hpet and lapic_base live in the kernel half, which create_addrspace()
     * shares with every process. */

    klogd("PROC: child pid %ld and parent pid %ld\n", tc->pid, tp->pid);
    spinlock_acquire(&tp->child_lock);
    vec_push_back(&tp->child_list, tc->pid);
    spinlock_release(&tp->child_lock);

  norm_exit:
    return tc;
}

void process_free(process_t * t)
{
    if (t->mode != PROC_USER_MODE) {
        kpanic("Process: cannot free kernel process %ld\n", t->pid);
    }

    process_table_remove(t);

    uint64_t mmap_num = vec_length(&t->mmap_list);
    for (uint64_t i = 0; i < mmap_num; i++) {
        mem_map_t m = vec_at(&t->mmap_list, i);
        vmm_unmap(t->addrspace, m.vaddr, m.np);
        kmfree_chunk((void *) PHYS_TO_VIRT(m.paddr), __func__, __LINE__);
    }
    vec_erase_all(&t->mmap_list);
    vec_erase_all(&t->child_list);

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

    handle_table_destroy(&t->handles);

    if (t->bootinfo != NULL)
        kmfree(t->bootinfo);

    kmfree(t);
}
