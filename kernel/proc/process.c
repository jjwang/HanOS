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
#include <stdint.h>
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

/* Replace the process working directory with a copy of cwd (or "/"). */
void process_set_cwd(process_t * t, const char *cwd)
{
    char *dup;

    if (cwd == NULL)
        cwd = "/";
    dup = kmalloc(strlen(cwd) + 1);
    if (dup == NULL)
        return;
    strcpy(dup, cwd);
    if (t->cwd != NULL)
        kmfree(t->cwd);
    t->cwd = dup;
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
    nproc->tgid = new_pid;
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

        /* The user stack VA equals its physical address. The kernel reaches it
         * through the direct map, and the process through an identity mapping. */
        if (pas != NULL) {
            vmm_map(pas, (uint64_t) nproc->ustack_limit,
                    (uint64_t) nproc->ustack_limit,
                    NUM_PAGES(STACK_SIZE),
                    VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);
        }

        vmm_map(as, (uint64_t) nproc->ustack_limit,
                (uint64_t) nproc->ustack_limit,
                NUM_PAGES(STACK_SIZE),
                VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);

        mem_map_t m;

        m.vaddr = (uint64_t) nproc->ustack_limit;
        m.paddr = (uint64_t) nproc->ustack_limit;
        m.np = NUM_PAGES(STACK_SIZE);
        m.flags = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;

        vec_push_back(&as->mmap_list, m);

        nproc_regs = (process_regs_t *)
            PHYS_TO_VIRT((uint64_t) nproc->ustack_top
                         - sizeof(process_regs_t));

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
    if (mode == PROC_USER_MODE) {
        /* context holds a physical address; exit_context_switch runs on the
         * identity-mapped stack after the CR3 switch. */
        nproc->context = (void *)((uint64_t) nproc->ustack_top
                                  - sizeof(process_regs_t));
    } else {
        nproc->context = nproc_regs;
    }
    nproc->ppid = PID_MAX;
    nproc->priority = priority;
    nproc->last_tick = 0;
    nproc->status = PROC_READY;

    process_set_cwd(nproc, "/");
    strncpy(nproc->name, name, sizeof(nproc->name));

    handle_table_init(&nproc->handles);
    klogi("PROC: Create pid %ld with name \"%s\" (process 0x%016lx)\n",
          nproc->pid, name, nproc);

    if (mode == PROC_USER_MODE && pas != NULL) {
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

    handle_table_init(&tc->handles);
    tc->cwd = NULL;
    process_set_cwd(tc, tp->cwd);

    pid_t new_pid = __atomic_fetch_add(&curr_pid, 1, __ATOMIC_RELAXED);

    tc->forked = true;
    tc->addrspace = create_addrspace();

    uint64_t len = vec_length(&(tp->addrspace->mmap_list));
    klogi("process_fork: totally %ld memory blocks (parent #%ld, child #%ld)\n",
          len, tp->pid, new_pid);

    uint64_t i;
    for (i = 0; i < len; i++) {
        mem_map_t m = vec_at(&(tp->addrspace->mmap_list), i);
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
        vec_push_back(&tc->addrspace->mmap_list, m);
    }

    tc->pid = new_pid;
    tc->ppid = tp->pid;
    tc->tgid = new_pid;
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

    /* The process server clones the child's fd table after the fork; k_fork()
     * drives that and the child waits until it is done. */
    tc->fds_ready = false;

    /* hpet and lapic_base live in the kernel half, which create_addrspace()
     * shares with every process. */

    klogd("PROC: child pid %ld and parent pid %ld\n", tc->pid, tp->pid);

  norm_exit:
    return tc;
}

/* Create a thread that shares tp's address space. frame points at tp's saved
 * syscall register frame (rax first). */
process_t *process_clone(process_t * tp, uint64_t flags, uint64_t stack,
                         uint64_t tls, int32_t * ctid, void *frame)
{
    process_t *tc = (process_t *) kmalloc(sizeof(process_t));
    if (tc == NULL)
        return NULL;

    memcpy(tc, tp, sizeof(process_t));

    tc->cwd = NULL;
    process_set_cwd(tc, tp->cwd);

    pid_t tid = __atomic_fetch_add(&curr_pid, 1, __ATOMIC_RELAXED);
    if (tid >= PID_MAX) {
        kmfree(tc);
        return NULL;
    }

    tc->pid = tid;
    tc->ppid = tp->pid;
    tc->tgid = tp->tgid;
    tc->is_thread = true;
    tc->forked = false;
    tc->status = PROC_READY;
    tc->fds_ready = true;

    /* Share the address space. */
    __atomic_fetch_add(&tp->addrspace->refs, 1, __ATOMIC_ACQ_REL);
    tc->addrspace = tp->addrspace;

    /* A thread gets fresh capability handles and no bootinfo. */
    handle_table_init(&tc->handles);
    tc->bootinfo = NULL;

    tc->kstack_limit = kmalloc_chunk(STACK_SIZE, __func__, __LINE__);
    tc->kstack_top = tc->kstack_limit + STACK_SIZE;

    /* Build the child's user-mode return frame on its kernel stack. Layout
     * matches pop_all + iretq: rax..r15, rip, cs, rflags, rsp, ss. */
    uint64_t *f = (uint64_t *) ((uint64_t) tc->kstack_top - 20 * 8);
    uint64_t *p = (uint64_t *) frame;
    uint64_t i;

    for (i = 0; i < 15; i++)
        f[i] = p[i];
    f[0] = 0;                   /* child returns 0 */
    f[15] = p[15];              /* resume after the syscall */
    f[16] = DEFAULT_UMODE_CODE;
    f[17] = p[17];
    f[18] = stack;
    f[19] = DEFAULT_UMODE_DATA;
    tc->context = f;

    tc->fs_base = (flags & CLONE_SETTLS) ? tls : tp->fs_base;
    tc->clear_child_tid = (flags & CLONE_CHILD_CLEARTID) ? ctid : NULL;

    process_table_add(tc);
    klogi("PROC: clone thread pid %ld in group %ld\n", tc->pid, tc->tgid);

    return tc;
}

void process_free(process_t * t)
{
    if (t->mode != PROC_USER_MODE) {
        kpanic("Process: cannot free kernel process %ld\n", t->pid);
    }

    process_table_remove(t);

    kmfree_chunk((void *) t->kstack_limit, __func__, __LINE__);

    /* Threads share the address space. Free it and its mappings only after the
     * last thread leaves. */
    if (t->addrspace != NULL
        && __atomic_fetch_sub(&t->addrspace->refs, 1, __ATOMIC_ACQ_REL) == 1) {
        uint64_t mmap_num = vec_length(&t->addrspace->mmap_list);

        for (uint64_t i = 0; i < mmap_num; i++) {
            mem_map_t m = vec_at(&t->addrspace->mmap_list, i);

            vmm_unmap(t->addrspace, m.vaddr, m.np);
            kmfree_chunk((void *) PHYS_TO_VIRT(m.paddr), __func__, __LINE__);
        }
        vec_erase_all(&t->addrspace->mmap_list);

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
    }

    handle_table_destroy(&t->handles);

    if (t->cwd != NULL)
        kmfree(t->cwd);

    if (t->bootinfo != NULL)
        kmfree(t->bootinfo);

    kmfree(t);
}
