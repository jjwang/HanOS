/**-----------------------------------------------------------------------------

 @file    sched.c
 @brief   Maintain process list and schedule processes according to system clock ticks
 @details
 @verbatim

  This file provides the implementation of the scheduler for the HanOS kernel.
  It handles context switching, scheduling algorithms, and process management.
  The scheduler is responsible for switching processes based on system clock ticks
  and other scheduling criteria. It also provides mechanisms for process creation,
  process state management, and synchronization.

  History:
  Apr 20, 2022 - 1. Redesign the process queue based on vector data structure.
                 2. Scheduler starts working after all processors are launched
                    to avoid GPF exception.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/klog.h>
#include <base/klib.h>
#include <base/time.h>
#include <base/kmalloc.h>
#include <base/vector.h>
#include <base/hash.h>
#include <proc/sched.h>
#include <proc/elf.h>
#include <sys/smp.h>
#include <sys/timer.h>
#include <sys/apic.h>
#include <sys/hpet.h>
#include <sys/pit.h>
#include <sys/isr_base.h>
#include <sys/idt.h>
#include <sys/panic.h>
#include <sys/cpu.h>
#include <sys/serial.h>
#include <libc/printf.h>

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

/* Per-CPU scheduler state. Each core owns a run queue holding the processes
 * assigned to it (ready, sleeping or dying) plus the process currently running.
 * Global pid lookup goes through the process table in process.c. */
static process_t *running_task[CPU_MAX] = { 0 };
static process_t *idle_task[CPU_MAX] = { 0 };
static uint64_t tick_count[CPU_MAX] = { 0 };
static spinlock_t run_queue_lock[CPU_MAX] = {0};

/* Vector used to notify a core that new work was queued on it. */
static uint8_t sched_ipi_vector = 0;

static volatile uint16_t cpu_num = 0;

typedef vec_struct(process_t*) process_vector_t;
process_vector_t run_queues[CPU_MAX] = {0};

/* Optional hook run just before a freshly exec'd process is made runnable, so a
 * spawner can attach granted resources without racing with the new process. */
static void (*sched_spawn_hook)(process_t * tc) = NULL;

void sched_set_spawn_hook(void (*hook) (process_t *))
{
    sched_spawn_hook = hook;
}

extern void enter_context_switch(void *v);
extern void exit_context_switch(void *stack, uint64_t cr3val);
extern void force_context_switch(void);
extern void fork_context_switch(void);

extern addrspace_t kaddrspace;

/* Pick the core a newly created process should be dispatched to. Processes are
 * spread across all online cores in a round-robin fashion so that a single
 * core does not have to run the whole workload.
 */
static uint16_t sched_pick_cpu(void)
{
    smp_info_t *info = smp_get_info();
    uint16_t ids[CPU_MAX];
    uint16_t n = 0;

    if (info != NULL) {
        for (uint16_t i = 0; i < info->num_cpus && i < CPU_MAX; i++)
            ids[n++] = info->cpus[i].cpu_id;
    }
    if (n == 0) {
        ids[0] = smp_get_current_cpu_id();
        n = 1;
    }

    static uint32_t rr_index;
    uint16_t idx =
        (uint16_t) (__atomic_fetch_add(&rr_index, 1, __ATOMIC_RELAXED) % n);

    return ids[idx];
}

/* Locate a process by its pid through the global process table. The returned
 * pointer is only valid until the process is reaped.
 */
static process_t *sched_find_task(pid_t pid)
{
    return process_lookup(pid);
}

_Noreturn void process_idle(pid_t pid)
{
    /* TODO: need to check why there will be #PF exception without sleeping. */
    //hpet_sleep(100);

    /* Need to determine the root cause why we need sleep here on SMP.
     * Page Fault exception will occur if there is no sleep here.
     */
    cpu_t *cpu = smp_get_current_cpu(true);
    ASSERT (cpu != NULL);
    
    uint16_t cpu_id = cpu->cpu_id;
    (void) pid;

    while (true) {
        process_t *t = NULL;

        /* Step 1: Find a dead process in this core's run queue */
        spinlock_acquire(&(run_queue_lock[cpu_id]));
        uint64_t task_num = vec_length(&run_queues[cpu_id]);
        for (uint64_t i = 0; i < task_num; i++) {
            process_t *cur = vec_at(&run_queues[cpu_id], i);
            if (cur != NULL && cur->status == PROC_DEAD) {
                vec_erase(&run_queues[cpu_id], i);
                t = cur;
                break;
            }
        }
        spinlock_release(&(run_queue_lock[cpu_id]));

        if (t == NULL) {
            /* If we cannot find dead processes, then fall into sleep */
            asm volatile ("hlt");
            continue;
        }

        /* Step 2: Remove the process from its parent's child list. The parent
         * may live on another core, so search all cores. */
        process_t *tp = sched_find_task(t->ppid);
        if (tp != NULL) {
            spinlock_acquire(&tp->child_lock);
            for (uint64_t k = 0; k < vec_length(&tp->child_list); k++) {
                if (vec_at(&tp->child_list, k) == t->pid) {
                    vec_erase(&tp->child_list, k);
                    break;
                }
            }
            if (vec_length(&tp->child_list) == 0
                && tp->status == PROC_DYING) {
                tp->status = PROC_DEAD;
            }
            spinlock_release(&tp->child_lock);
        }

        klogi("sched: clean memory of dead process #%ld (0x%016lx)\n", t->pid, t);

        /* Step 3: Free all resources of this dead process */
        process_free(t);
    }
}

/*
 * Context switch has 3 situations which are determined by parameter "mode":
 * SCHED_SWITCH_TIME_CYCLE(0): triggered by timer cycle.
 * SCHED_SWITCH_SLEEP     (1): triggered by process itself which needs to fall in
 * sleep.
 * SCHED_SWITCH_FORK      (2): triggered by fork which needs to create a clone.
 *
 */
void do_context_switch(void *stack, int64_t mode)
{
    /* If all CPUs initialization are not finished, smp_info will be NULL
     * here
     */
    smp_info_t *smp_info = smp_get_info();
    uint16_t cpu_id = smp_get_current_cpu_id();
    if (smp_info == NULL) {
        kpanic("sched: CPU %ld cannot get SMP information\n", cpu_id);
        return;
    }

    cpu_t *cpu = smp_get_current_cpu(true);
    uint64_t ticks = tick_count[cpu_id];

    spinlock_acquire(&(run_queue_lock[cpu_id]));

    process_t *curr = running_task[cpu_id];

    if (curr != NULL) {
        /* Process status: 0 - ready, 1 - running, 2 - sleeping */
        curr->context = stack;
        curr->last_tick = ticks;
        curr->errno = cpu->errno;

        if ((uint64_t) curr != (uint64_t) idle_task[cpu_id]) {
            if (mode == SCHED_SWITCH_FORK) {
                process_t *curr_fork = process_fork(curr);
                if (curr_fork->status == PROC_RUNNING)
                    curr_fork->status = PROC_READY;
                vec_push_back(&run_queues[cpu_id], curr_fork);
                curr->fork_retval = curr_fork->pid;
                curr_fork->fork_retval = 0;
            }
            if (curr->status != PROC_RUNNING) {
                vec_push_back(&run_queues[cpu_id], curr);
                running_task[cpu_id] = NULL;
                curr = NULL;
            }
        } else {
            curr = NULL;
        }
    }

    process_t *next = NULL;
    int64_t tasks_num = vec_length(&run_queues[cpu_id]);

    /* Prefer runnable processes and only fall back to a process whose sleep has
     * expired when no ready process exists. Otherwise a process that performs
     * sched_sleep(0) as a yield can starve ready processes queued behind it
     * (e.g. a process dispatched to this core by another core).
     */
    for (int64_t i = 0; i < tasks_num; i++) {
        process_t *t = vec_at(&run_queues[cpu_id], i);
        if (t->status == PROC_READY) {
            next = t;
            vec_erase(&run_queues[cpu_id], i);
            break;
        }
    }

    if (next == NULL) {
        for (int64_t i = 0; i < tasks_num; i++) {
            process_t *t = vec_at(&run_queues[cpu_id], i);
            if (t->status == PROC_SLEEPING
                && t->wakeup_time > 0
                && hpet_get_nanos() >= t->wakeup_time) {
                next = t;
                vec_erase(&run_queues[cpu_id], i);
                break;
            }
        }
    }

    if (next != NULL) {
        if (curr != NULL) {
            curr->status = PROC_READY;
            vec_push_back(&run_queues[cpu_id], curr);
        }
    } else {
        next = (curr == NULL) ? idle_task[cpu_id] : curr;
    }

    next->status = PROC_RUNNING;
    running_task[cpu_id] = next;

    cpu->errno = next->errno;
    cpu->tss.rsp0 = (uint64_t) next->kstack_top;

    tick_count[cpu_id]++;

    if (!(cpu->tss.rsp0 & 0xFFFF000000000000) || next->pid < 1) {
        kpanic("SCHED: CPU %ld kernel stack 0x%016lx addrspace 0x%016lx corrputed "
               "(kernel 0x%016lx|0x%016lx user 0x%016lx|%016lx in process 0x%016lx pid %ld, last tick %ld)\n",
               cpu->cpu_id, cpu->tss.rsp0, next->addrspace,
               next->kstack_top, next->kstack_limit, next->ustack_top,
               next->ustack_limit, next, next->pid, next->last_tick);
    }

    if (next->fs_base != 0 && read_msr(MSR_FS_BASE) != next->fs_base) {
        /*
         * Here we must set to the corresponding correct FS_BASE, else it will
         * bring Page Fault exception.
         */
        write_msr(MSR_FS_BASE, next->fs_base);
    }

    if (mode == SCHED_SWITCH_TIME_CYCLE) {
        apic_send_eoi();
    }

    spinlock_release(&(run_queue_lock[cpu_id]));

    exit_context_switch(next->context, (next->addrspace == NULL)
                        ? VIRT_TO_PHYS((uint64_t) kaddrspace.PML4)
                        : VIRT_TO_PHYS((uint64_t) next->addrspace->PML4));
}

pid_t sched_get_pid()
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return PID_MAX;
    }

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_task[cpu_id];
    pid_t pid = curr->pid;

    if (pid < 1)
        kpanic("SCHED: %s returns corrupted pid\n", __func__);

    return pid;
}

pid_t sched_fork(void)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) return PID_MAX;
    uint16_t cpu_id = cpu->cpu_id;
    if (running_task[cpu_id] && running_task[cpu_id]->pid < 1)
        kpanic("SCHED: %s meets corrupted pid\n", __func__);
    fork_context_switch();
    return running_task[cpu_id]->fork_retval;
}

void sched_sleep_impl(time_t millis, bool advanced)
{
    if (millis != 0 && advanced) {
        hpet_sleep(millis);
        return;
    }

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_task[cpu_id];
    if (curr) {
        curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
        curr->wakeup_event.type = EVENT_UNDEFINED;
        curr->status = PROC_SLEEPING;
        if (curr->pid < 1) {
            kpanic("SCHED: %s meets corrupted pid\n", __func__);
        }
    }

    force_context_switch();
}

/* Report the status of a process. A process that still has a live child is reported
 * as PROC_RUNNING so that it is not reaped before its children.
 */
process_status_t sched_get_task_status(pid_t pid)
{
    process_t *t = process_lookup(pid);
    if (t == NULL)
        return PROC_UNKNOWN;

    bool has_child = false;

    spinlock_acquire(&t->child_lock);
    for (uint64_t i = 0; i < vec_length(&t->child_list); i++) {
        pid_t child_pid = vec_at(&t->child_list, i);
        process_t *child = process_lookup(child_pid);
        if (child != NULL && child->status != PROC_DEAD
            && child->status != PROC_UNKNOWN) {
            has_child = true;
            break;
        }
    }
    spinlock_release(&t->child_lock);

    if (has_child)
        return PROC_RUNNING;
    if (t->status == PROC_DYING)
        return PROC_DEAD;

    return t->status;
}

/* Wake up a parent that is sleeping in waitpid() for one of its children to
 * exit. The parent re-checks its child list after waking, so it is enough to
 * mark every sleeper that waits on EVENT_CHILD_EXIT as ready.
 */
static void sched_wake_child_waiter(pid_t parent_pid)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_task[c] == NULL && running_task[c] == NULL)
            continue;

        spinlock_acquire(&run_queue_lock[c]);

        for (uint64_t i = 0; i < vec_length(&run_queues[c]); i++) {
            process_t *t = vec_at(&run_queues[c], i);
            if (t != NULL && t->pid == parent_pid
                && t->status == PROC_SLEEPING
                && t->wakeup_event.type == EVENT_CHILD_EXIT) {
                t->wakeup_time = 0;
                t->status = PROC_READY;
            }
        }

        spinlock_release(&run_queue_lock[c]);
    }
}

void sched_exit(int64_t status)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return;
    }

    /* The DYING -> DEAD transition must not be preempted: a process that is
     * switched out while PROC_DYING is never selected again, so it would stay
     * DYING forever and its parent would keep seeing a live child. Keep
     * interrupts disabled until the final context switch. */
    asm volatile ("cli" ::: "memory");

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_task[cpu_id];
    if (curr) {
        curr->status = PROC_DYING;
        curr->exit_status = status;
        if (curr->pid < 1) {
            kpanic("SCHED: %s meets corrupted pid\n", __func__);
        }
        spinlock_acquire(&curr->child_lock);
        uint64_t len = vec_length(&(curr->child_list));
        pid_t *children = NULL;
        if (len > 0) {
            children = kmalloc(len * sizeof(pid_t));
            if (children != NULL) {
                for (uint64_t i = 0; i < len; i++)
                    children[i] = vec_at(&(curr->child_list), i);
            }
        }
        spinlock_release(&curr->child_lock);

        bool all_children_dead = (children != NULL || len == 0);
        for (uint64_t i = 0; all_children_dead && i < len; i++) {
            if (sched_get_task_status(children[i]) != PROC_DEAD) {
                all_children_dead = false;
            }
        }
        if (children != NULL) {
            kmfree(children);
        }

        if (all_children_dead) {  /* This also includes no-children situation */
            curr->status = PROC_DEAD;
        }

        for (uint64_t i = 0; i < curr->open_files_table.size; i++) {
            if (curr->open_files_table.array[i].key == -1
                || curr->open_files_table.array[i].data == NULL) {
                continue;
            }
            klogd("sched_exit: dead process pid %ld close file handle %ld\n",
                  curr->pid, curr->open_files_table.array[i].key);
            vfs_close(curr->open_files_table.array[i].key);
        }
        if (curr->open_files_table.array != NULL) {
            kmfree(curr->open_files_table.array);
            curr->open_files_table.array = NULL;
        }
        curr->open_files_table.size = 0;

        sched_wake_child_waiter(curr->ppid);
    }

    force_context_switch();
}

/* Sleep until a child of the current process exits, or until the timeout
 * expires. The timeout guarantees progress even if the wakeup is missed
 * because the child exited just before this process went to sleep.
 */
void sched_wait_child(time_t millis)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_task[cpu_id];
    if (curr == NULL) {
        return;
    }

    curr->wakeup_event.type = EVENT_CHILD_EXIT;
    curr->wakeup_event.para = 0;
    curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
    curr->status = PROC_SLEEPING;

    force_context_switch();
}

/* Block the current process until sched_wake_key(key) is called or the timeout
 * expires. Used by the IPC receive path. */
void sched_wait_key(void *key, time_t millis)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_task[cpu_id];
    if (curr == NULL)
        return;

    curr->wakeup_event.type = EVENT_IPC;
    curr->wakeup_event.para = 0;
    curr->wakeup_key = key;
    curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
    curr->status = PROC_SLEEPING;

    force_context_switch();
}

/* Wake every process sleeping on the given key, on any core. */
void sched_wake_key(void *key)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_task[c] == NULL && running_task[c] == NULL)
            continue;

        spinlock_acquire(&run_queue_lock[c]);

        for (uint64_t i = 0; i < vec_length(&run_queues[c]); i++) {
            process_t *t = vec_at(&run_queues[c], i);
            if (t != NULL && t->status == PROC_SLEEPING
                && t->wakeup_event.type == EVENT_IPC
                && t->wakeup_key == key) {
                t->wakeup_time = 0;
                t->status = PROC_READY;
            }
        }

        spinlock_release(&run_queue_lock[c]);
    }
}

process_t *sched_get_current_process()
{
    cpu_t *cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return NULL;
    }

    return running_task[cpu->cpu_id];
}

uint64_t sched_get_ticks()
{
    cpu_t *cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return 0;
    }

    return tick_count[cpu->cpu_id];
}

void sched_init(const char *name, uint16_t cpu_id)
{
    idle_task[cpu_id] = process_make(name, process_idle, 255,
                                   PROC_KERNEL_MODE, NULL);

    klogi("SCHED: create idle process 0x%016lx with pid %ld for CPU %ld\n",
        idle_task[cpu_id], idle_task[cpu_id]->pid, cpu_id);

    vec_push_back(&run_queues[cpu_id], idle_task[cpu_id]);

    apic_timer_init(cpu_id);
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);

    /* A core waiting in its idle loop relies on its own timer to notice a process
     * queued by another core. Reserving a separate vector lets a dispatcher
     * kick the target core directly instead. */
    if (sched_ipi_vector == 0) {
        sched_ipi_vector = idt_get_available_vector();
        idt_set_handler(sched_ipi_vector, enter_context_switch);
    }

    cpu_num++;

    klogi
        ("SCHED: initialization finished for CPU %ld with idle process %s:%ld\n",
         cpu_id, name, idle_task[cpu_id]->pid);
}

uint16_t sched_get_cpu_num()
{
    return cpu_num;
}

process_t *sched_new(const char *name, void (*entry)(pid_t),
                  bool usermode)
{
    process_t *t = process_make(name, entry, 0,
                          usermode ? PROC_USER_MODE : PROC_KERNEL_MODE,
                          NULL);

    return t;
}

void sched_add(process_t *t)
{
    uint16_t target = sched_pick_cpu();
    uint16_t current = smp_get_current_cpu_id();

    spinlock_acquire(&run_queue_lock[target]);
    vec_push_back(&run_queues[target], t);
    spinlock_release(&run_queue_lock[target]);

    /* Wake the target core if it is not this one; otherwise it only finds the
     * new process on its next timer tick. */
    if (target != current && sched_ipi_vector != 0)
        apic_send_ipi(target, sched_ipi_vector, 0);

    klogi("SCHED: CPU %ld dispatches pid %ld to CPU %ld\n",
          smp_get_current_cpu_id(), t->pid, target);
}

process_t *sched_execve(const char *path, const char *argv[],
                     const char *envp[], const char *cwd)
{
    int64_t i;

    klogi("SCHED: execute \"%s\" in \"%s\" directory\n", path, cwd);

    auxval_t aux = { 0 };
    uint64_t entry = 0;

    process_t *tp = sched_get_current_process();
    process_t *tc = NULL;

    char *tname = (char *) path;
    for (i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            tname = (char *) &(path[i + 1]);
            break;
        }
    }

    tc = process_make(tname, NULL, 0, PROC_USER_MODE,
                   tp == NULL ? NULL : tp->addrspace);

    if (tp != NULL) {
        uint64_t i;

        for (i = 0; i < vec_length(&tp->dup_list); i++) {
            file_dup_t dup = vec_at(&tp->dup_list, i);
            vec_push_back(&tc->dup_list, dup);
            klogd("SCHED: fh pair for pid %ld's child process %ld - (%ld, %ld)\n",
                  tp->pid, tc->pid, dup.fh, dup.newfh);
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
            tc->open_files_table.array[i] = tp->open_files_table.array[i];
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
            klogd("SCHED: copy fd %ld from pid %ld to pid %ld\n",
                  tc->open_files_table.array[i].key, tp->pid, tc->pid);
        }
    }

    if (elf_load(tc, path, &entry, &aux)) {
        /* Need to release memory for process "tc" */
        process_free(tc);
        return NULL;
    }

    process_regs_t *tc_regs = (process_regs_t *) PHYS_TO_VIRT(tc->context);

    /* TODO: Do not check whether aux.entry == entry any more */
    uint64_t *stack = (uint64_t *) PHYS_TO_VIRT(tc->context);

    if (cwd != NULL)
        strcpy(tc->cwd, cwd);

    uint8_t *sa = (uint8_t *) tc->context;
    uint64_t nenv = 0, nargs = 0;

    if (argv != NULL && envp != NULL) {
        uint64_t i = 0;
        const char *e;
        for (i = 0;; i++) {
            e = envp[i];
            if (e == NULL)
                break;
            stack = (void *) stack - (strlen(e) + 1);
            strcpy((char *) stack, e);
            klogd("         envp: %s (0x%016lx -> 0x%016lx, %ld)\n",
                  e, e, stack, strlen(e) + 1);
            nenv++;
        }

        for (i = 0;; i++) {
            e = argv[i];
            if (e == NULL)
                break;
            stack = (void *) stack - (strlen(e) + 1);
            strcpy((char *) stack, e);
            klogd("         argv: %s (0x%016lx -> 0x%016lx, %ld)\n",
                  e, e, stack, strlen(e) + 1);
            nargs++;
        }

        /* Align stack address to 16-byte */
        stack = (void *) stack - ((uintptr_t) stack & 0xf);

        if ((nargs + nenv + 1) & 1)
            stack--;
    } else {
        *(--stack) = 0;
    }

    /* Auxilary vector */
    *(--stack) = 0;
    *(--stack) = 0;

    stack -= 2;
    stack[0] = 10;              /* AT_ENTRY */
    stack[1] = aux.entry;

    stack -= 2;
    stack[0] = 20;              /* AT_PHDR */
    stack[1] = aux.phdr;

    stack -= 2;
    stack[0] = 21;              /* AT_PHENT */
    stack[1] = aux.phentsize;

    stack -= 2;
    stack[0] = 22;              /* AT_PHNUM */
    stack[1] = aux.phnum;

    klogi
        ("SCHED: pid %ld aux stack 0x%016lx (RSP 0x%016lx), entry 0x%016lx, phdr 0x%016lx, "
         "phentsize %ld, phnum %ld\n", tc->pid, stack, tc_regs->rsp,
         aux.entry, aux.phdr, aux.phentsize, aux.phnum);

    /* Environment variables */
    *(--stack) = 0;             /* End of environment */

    if (argv != NULL && envp != NULL) {
        stack -= nenv;
        for (uint64_t i = 0; i < nenv; i++) {
            sa -= strlen(envp[i]) + 1;
            stack[i] = (uint64_t) sa;
        }
    }

    /* Arguments */
    *(--stack) = 0;             /* End of arguments */

    if (argv != NULL && envp != NULL) {
        stack -= nargs;
        for (uint64_t i = 0; i < nargs; i++) {
            sa -= strlen(argv[i]) + 1;
            stack[i] = (uint64_t) sa;
        }
        *(--stack) = nargs;     /* argc */
    } else {
        *(--stack) = 0;
    }

    stack = (uint64_t *) ((uint64_t) stack - sizeof(process_regs_t));
    memcpy(stack, tc_regs, sizeof(process_regs_t));

    tc->context = (void *) VIRT_TO_PHYS(stack);
    tc_regs = (process_regs_t *) stack;
    tc_regs->rsp = (uint64_t) tc->context + sizeof(process_regs_t);

    klogd("SCHED: process stack top 0x%016lx, rsp 0x%016lx, top argc %ld\n",
          tc->context, tc_regs->rsp,
          *((uint64_t *) PHYS_TO_VIRT(tc_regs->rsp)));

    /* --- Stack filling finished --- */

    tc_regs->rip = (uint64_t) entry;

    klogd("SCHED: finished initialization with entry 0x%016lx\n", entry);

    if (tp != NULL) {
        klogi("SCHED: child pid %ld and parent pid %ld\n", tc->pid, tp->pid);
        spinlock_acquire(&tp->child_lock);
        vec_push_back(&tp->child_list, tc->pid);
        spinlock_release(&tp->child_lock);
        tc->ppid = tp->pid;
    }

    if (sched_spawn_hook != NULL)
        sched_spawn_hook(tc);

    sched_add(tc);

    return tc;
}

/* Try to reap a process that has already exited.
 *
 * Returns 1 and stores the process's exit status in *status when a dead process was
 * found, removed from its run queue and freed; 0 when the process still exists but
 * is alive; -1 when no process with that pid can be found (it was already reaped
 * by another core's idle process).
 */
int sched_reap(pid_t pid, int64_t *status)
{
    /* Use the effective status: a PROC_DYING process that has no live children is
     * reported as PROC_DEAD, so an exec wrapper whose replacement has exited
     * can be reaped even if the idle process never finalized it. */
    process_status_t st = sched_get_task_status(pid);

    if (st == PROC_UNKNOWN)
        return -1;              /* already reaped */
    if (st != PROC_DEAD)
        return 0;               /* still alive */

    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_task[c] == NULL && running_task[c] == NULL)
            continue;

        spinlock_acquire(&run_queue_lock[c]);

        process_t *rt = running_task[c];
        if (rt != NULL && rt->pid == pid) {
            /* Still running (possibly about to finalize its own exit). */
            spinlock_release(&run_queue_lock[c]);
            return 0;
        }

        for (uint64_t i = 0; i < vec_length(&run_queues[c]); i++) {
            process_t *t = vec_at(&run_queues[c], i);
            if (t == NULL || t->pid != pid)
                continue;

            bool dead = (t->status == PROC_DEAD || t->status == PROC_DYING);
            int64_t exit_status = t->exit_status;

            if (!dead) {
                spinlock_release(&run_queue_lock[c]);
                return 0;
            }

            vec_erase(&run_queues[c], i);
            spinlock_release(&run_queue_lock[c]);

            klogi("SCHED: CPU %ld reaps dead process #%ld from CPU %ld\n",
                  smp_get_current_cpu_id(), pid, c);
            if (status != NULL)
                *status = exit_status;
            process_free(t);
            return 1;
        }

        spinlock_release(&run_queue_lock[c]);
    }

    return -1;
}

