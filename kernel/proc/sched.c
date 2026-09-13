/**-----------------------------------------------------------------------------

 @file    sched.c
 @brief   Maintain task list and schedule tasks according to system clock ticks
 @details
 @verbatim

  This file provides the implementation of the scheduler for the HanOS kernel.
  It handles context switching, scheduling algorithms, and task management.
  The scheduler is responsible for switching tasks based on system clock ticks
  and other scheduling criteria. It also provides mechanisms for task creation,
  task state management, and synchronization.

  History:
  Apr 20, 2022 - 1. Redesign the task queue based on vector data structure.
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
#include <proc/eventbus.h>
#include <sys/smp.h>
#include <sys/timer.h>
#include <sys/apic.h>
#include <sys/hpet.h>
#include <sys/pit.h>
#include <sys/isr_base.h>
#include <sys/panic.h>
#include <sys/cpu.h>
#include <sys/serial.h>
#include <libc/printf.h>

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

static task_t *tasks_running[CPU_MAX] = { 0 };
static task_t *tasks_idle[CPU_MAX] = { 0 };
static uint64_t tasks_coordinate[CPU_MAX] = { 0 };
static spinlock_t tasks_lock[CPU_MAX] = {0};

static volatile uint16_t cpu_num = 0;

typedef vec_struct(task_t*) task_vector_t;
task_vector_t tasks_active_table[CPU_MAX] = {0};

extern void enter_context_switch(void *v);
extern void exit_context_switch(task_t * next, uint64_t cr3val);
extern void force_context_switch(void);
extern void fork_context_switch(void);

extern addrspace_t kaddrspace;

/* Pick the core a newly created task should be dispatched to. Tasks are
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

/* Locate a task by its tid on any core. The returned pointer is only valid
 * until the task is reaped, so callers that need to keep it must ensure the
 * task cannot be freed concurrently (e.g. because it still has children).
 */
static task_t *sched_find_task(task_id_t tid)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (tasks_idle[c] == NULL && tasks_running[c] == NULL)
            continue;

        spinlock_acquire(&tasks_lock[c]);

        task_t *rt = tasks_running[c];
        if (rt != NULL && rt->tid == tid) {
            spinlock_release(&tasks_lock[c]);
            return rt;
        }

        for (uint64_t i = 0; i < vec_length(&tasks_active_table[c]); i++) {
            task_t *t = vec_at(&tasks_active_table[c], i);
            if (t != NULL && t->tid == tid) {
                spinlock_release(&tasks_lock[c]);
                return t;
            }
        }

        spinlock_release(&tasks_lock[c]);
    }

    return NULL;
}

_Noreturn void task_idle_proc(task_id_t tid)
{
    /* TODO: need to check why there will be #PF exception without sleeping. */
    //hpet_sleep(100);

    /* Need to determine the root cause why we need sleep here on SMP.
     * Page Fault exception will occur if there is no sleep here.
     */
    cpu_t *cpu = smp_get_current_cpu(true);
    ASSERT (cpu != NULL);
    
    uint16_t cpu_id = cpu->cpu_id;
    (void) tid;

    while (true) {
        task_t *t = NULL;

        /* Step 1: Find a dead task in this core's run queue */
        spinlock_acquire(&(tasks_lock[cpu_id]));
        uint64_t task_num = vec_length(&tasks_active_table[cpu_id]);
        for (uint64_t i = 0; i < task_num; i++) {
            task_t *cur = vec_at(&tasks_active_table[cpu_id], i);
            if (cur != NULL && cur->status == TASK_DEAD) {
                vec_erase(&tasks_active_table[cpu_id], i);
                t = cur;
                break;
            }
        }
        spinlock_release(&(tasks_lock[cpu_id]));

        if (t == NULL) {
            /* If we cannot find dead tasks, then fall into sleep */
            asm volatile ("hlt");
            continue;
        }

        /* Step 2: Remove the task from its parent's child list. The parent
         * may live on another core, so search all cores. */
        task_t *tp = sched_find_task(t->ptid);
        if (tp != NULL) {
            spinlock_acquire(&tp->child_lock);
            for (uint64_t k = 0; k < vec_length(&tp->child_list); k++) {
                if (vec_at(&tp->child_list, k) == t->tid) {
                    vec_erase(&tp->child_list, k);
                    break;
                }
            }
            if (vec_length(&tp->child_list) == 0
                && tp->status == TASK_DYING) {
                tp->status = TASK_DEAD;
            }
            spinlock_release(&tp->child_lock);
        }

        klogi("sched: clean memory of dead task #%ld (0x%016lx)\n", t->tid, t);

        /* Step 3: Free all resources of this dead task */
        task_free(t);
    }
}

/*
 * Context switch has 3 situations which are determined by parameter "mode":
 * SCHED_SWITCH_TIME_CYCLE(0): triggered by timer cycle.
 * SCHED_SWITCH_SLEEP     (1): triggered by task itself which needs to fall in
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

    /* Firstly all events on event bus should be processed */
    eb_dispatch();

    cpu_t *cpu = smp_get_current_cpu(true);
    uint64_t ticks = tasks_coordinate[cpu_id];

    spinlock_acquire(&(tasks_lock[cpu_id]));

    task_t *curr = tasks_running[cpu_id];

    if (curr != NULL) {
        /* Task status: 0 - ready, 1 - running, 2 - sleeping */
        curr->tstack_top = stack;
        curr->last_tick = ticks;
        curr->errno = cpu->errno;

        if ((uint64_t) curr != (uint64_t) tasks_idle[cpu_id]) {
            if (mode == SCHED_SWITCH_FORK) {
                task_t *curr_fork = task_fork(curr);
                if (curr_fork->status == TASK_RUNNING)
                    curr_fork->status = TASK_READY;
                vec_push_back(&tasks_active_table[cpu_id], curr_fork);
                curr->fork_retval = curr_fork->tid;
                curr_fork->fork_retval = 0;
            }
            if (curr->status != TASK_RUNNING) {
                vec_push_back(&tasks_active_table[cpu_id], curr);
                tasks_running[cpu_id] = NULL;
                curr = NULL;
            }
        } else {
            curr = NULL;
        }
    }

    task_t *next = NULL;
    int64_t tasks_num = vec_length(&tasks_active_table[cpu_id]);

    /* Prefer runnable tasks and only fall back to a task whose sleep has
     * expired when no ready task exists. Otherwise a task that performs
     * sched_sleep(0) as a yield can starve ready tasks queued behind it
     * (e.g. a process dispatched to this core by another core).
     */
    for (int64_t i = 0; i < tasks_num; i++) {
        task_t *t = vec_at(&tasks_active_table[cpu_id], i);
        if (t->status == TASK_READY) {
            next = t;
            vec_erase(&tasks_active_table[cpu_id], i);
            break;
        }
    }

    if (next == NULL) {
        for (int64_t i = 0; i < tasks_num; i++) {
            task_t *t = vec_at(&tasks_active_table[cpu_id], i);
            if (t->status == TASK_SLEEPING
                && t->wakeup_time > 0
                && hpet_get_nanos() >= t->wakeup_time) {
                next = t;
                vec_erase(&tasks_active_table[cpu_id], i);
                break;
            }
        }
    }

    if (next != NULL) {
        if (curr != NULL) {
            curr->status = TASK_READY;
            vec_push_back(&tasks_active_table[cpu_id], curr);
        }
    } else {
        next = (curr == NULL) ? tasks_idle[cpu_id] : curr;
    }

    next->status = TASK_RUNNING;
    tasks_running[cpu_id] = next;

    cpu->errno = next->errno;
    cpu->tss.rsp0 = (uint64_t) next->kstack_top;

    tasks_coordinate[cpu_id]++;

    if (!(cpu->tss.rsp0 & 0xFFFF000000000000) || next->tid < 1) {
        kpanic("SCHED: CPU %ld kernel stack 0x%016lx addrspace 0x%016lx corrputed "
               "(kernel 0x%016lx|0x%016lx user 0x%016lx|%016lx in task 0x%016lx tid %ld, last tick %ld)\n",
               cpu->cpu_id, cpu->tss.rsp0, next->addrspace,
               next->kstack_top, next->kstack_limit, next->ustack_top,
               next->ustack_limit, next, next->tid, next->last_tick);
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

    spinlock_release(&(tasks_lock[cpu_id]));

    exit_context_switch(next->tstack_top, (next->addrspace == NULL)
                        ? VIRT_TO_PHYS((uint64_t) kaddrspace.PML4)
                        : VIRT_TO_PHYS((uint64_t) next->addrspace->PML4));
}

task_id_t sched_get_tid()
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return TID_MAX;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    task_id_t tid = curr->tid;

    if (tid < 1)
        kpanic("SCHED: %s returns corrupted tid\n", __func__);

    return tid;
}

task_id_t sched_fork(void)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) return TID_MAX;
    uint16_t cpu_id = cpu->cpu_id;
    if (tasks_running[cpu_id] && tasks_running[cpu_id]->tid < 1)
        kpanic("SCHED: %s meets corrupted tid\n", __func__);
    fork_context_switch();
    return tasks_running[cpu_id]->fork_retval;
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
    task_t *curr = tasks_running[cpu_id];
    if (curr) {
        curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
        curr->wakeup_event.type = EVENT_UNDEFINED;
        curr->status = TASK_SLEEPING;
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
    }

    force_context_switch();
}

/* Compute the status of a task, taking all cores into account. A task may
 * have been dispatched to a core other than the caller's, so every run queue
 * has to be inspected. A parent is reported as TASK_RUNNING as long as one
 * of its children is still alive.
 */
task_status_t sched_get_task_status(task_id_t tid)
{
    task_t *ntask = NULL;
    bool has_child = false;

    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (tasks_idle[c] == NULL && tasks_running[c] == NULL)
            continue;

        spinlock_acquire(&tasks_lock[c]);

        task_t *rt = tasks_running[c];
        if (rt != NULL) {
            if (rt->tid == tid)
                ntask = rt;
            if (rt->ptid == tid && rt->status != TASK_DEAD
                && rt->status != TASK_UNKNOWN)
                has_child = true;
        }

        for (uint64_t i = 0; i < vec_length(&tasks_active_table[c]); i++) {
            task_t *t = vec_at(&tasks_active_table[c], i);
            if (t == NULL)
                continue;
            if (t->tid == tid)
                ntask = t;
            if (t->ptid == tid && t->status != TASK_DEAD
                && t->status != TASK_UNKNOWN)
                has_child = true;
        }

        spinlock_release(&tasks_lock[c]);
    }

    if (has_child)
        return TASK_RUNNING;
    if (ntask == NULL)
        return TASK_UNKNOWN;
    if (ntask->status == TASK_DYING)
        return TASK_DEAD;

    return ntask->status;
}

/* Wake up a parent that is sleeping in waitpid() for one of its children to
 * exit. The parent re-checks its child list after waking, so it is enough to
 * mark every sleeper that waits on EVENT_CHILD_EXIT as ready.
 */
static void sched_wake_child_waiter(task_id_t parent_tid)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (tasks_idle[c] == NULL && tasks_running[c] == NULL)
            continue;

        spinlock_acquire(&tasks_lock[c]);

        for (uint64_t i = 0; i < vec_length(&tasks_active_table[c]); i++) {
            task_t *t = vec_at(&tasks_active_table[c], i);
            if (t != NULL && t->tid == parent_tid
                && t->status == TASK_SLEEPING
                && t->wakeup_event.type == EVENT_CHILD_EXIT) {
                t->wakeup_time = 0;
                t->status = TASK_READY;
            }
        }

        spinlock_release(&tasks_lock[c]);
    }
}

void sched_exit(int64_t status)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return;
    }

    /* The DYING -> DEAD transition must not be preempted: a task that is
     * switched out while TASK_DYING is never selected again, so it would stay
     * DYING forever and its parent would keep seeing a live child. Keep
     * interrupts disabled until the final context switch. */
    asm volatile ("cli" ::: "memory");

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    if (curr) {
        curr->status = TASK_DYING;
        curr->exit_status = status;
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
        spinlock_acquire(&curr->child_lock);
        uint64_t len = vec_length(&(curr->child_list));
        task_id_t *children = NULL;
        if (len > 0) {
            children = kmalloc(len * sizeof(task_id_t));
            if (children != NULL) {
                for (uint64_t i = 0; i < len; i++)
                    children[i] = vec_at(&(curr->child_list), i);
            }
        }
        spinlock_release(&curr->child_lock);

        bool all_children_dead = (children != NULL || len == 0);
        for (uint64_t i = 0; all_children_dead && i < len; i++) {
            if (sched_get_task_status(children[i]) != TASK_DEAD) {
                all_children_dead = false;
            }
        }
        if (children != NULL) {
            kmfree(children);
        }

        if (all_children_dead) {  /* This also includes no-children situation */
            curr->status = TASK_DEAD;
        }

        for (uint64_t i = 0; i < curr->open_files_table.size; i++) {
            if (curr->open_files_table.array[i].key == -1
                || curr->open_files_table.array[i].data == NULL) {
                continue;
            }
            klogd("sched_exit: dead task tid %ld close file handle %ld\n",
                  curr->tid, curr->open_files_table.array[i].key);
            vfs_close(curr->open_files_table.array[i].key);
        }
        if (curr->open_files_table.array != NULL) {
            kmfree(curr->open_files_table.array);
            curr->open_files_table.array = NULL;
        }
        curr->open_files_table.size = 0;

        sched_wake_child_waiter(curr->ptid);
    }

    force_context_switch();
}

bool sched_resume_event(event_t event)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    ASSERT (cpu != NULL);

    uint16_t cpu_id = cpu->cpu_id;

    bool ret = false;

    spinlock_acquire(&tasks_lock[cpu_id]);
    for (uint64_t i = 0; i < vec_length(&tasks_active_table[cpu_id]); i++) {
        task_t *t = vec_at(&tasks_active_table[cpu_id], i);
        if (t) {
            if (t->status == TASK_SLEEPING
                && t->wakeup_event.type == event.type) {
                t->status = TASK_READY;
                t->wakeup_event.para = event.para;
                ret = true;
            }
        }
    }
    spinlock_release(&tasks_lock[cpu_id]);

    return ret;
}

event_t sched_wait_event(event_t event)
{
    event_t e = { 0 };
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return e;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    curr->wakeup_time = 0;
    curr->wakeup_event = event;
    curr->status = TASK_SLEEPING;

    if (curr->tid < 1)
        kpanic("SCHED: %s meets corrupted tid\n", __func__);

    force_context_switch();

    return curr->wakeup_event;
}

/* Sleep until a child of the current task exits, or until the timeout
 * expires. The timeout guarantees progress even if the wakeup is missed
 * because the child exited just before this task went to sleep.
 */
void sched_wait_child(time_t millis)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    if (curr == NULL) {
        return;
    }

    curr->wakeup_event.type = EVENT_CHILD_EXIT;
    curr->wakeup_event.para = 0;
    curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
    curr->status = TASK_SLEEPING;

    force_context_switch();
}

/* Block the current task until sched_wake_key(key) is called or the timeout
 * expires. Used by the IPC receive path. */
void sched_wait_key(void *key, time_t millis)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    if (curr == NULL)
        return;

    curr->wakeup_event.type = EVENT_IPC;
    curr->wakeup_event.para = 0;
    curr->wakeup_key = key;
    curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
    curr->status = TASK_SLEEPING;

    force_context_switch();
}

/* Wake every task sleeping on the given key, on any core. */
void sched_wake_key(void *key)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (tasks_idle[c] == NULL && tasks_running[c] == NULL)
            continue;

        spinlock_acquire(&tasks_lock[c]);

        for (uint64_t i = 0; i < vec_length(&tasks_active_table[c]); i++) {
            task_t *t = vec_at(&tasks_active_table[c], i);
            if (t != NULL && t->status == TASK_SLEEPING
                && t->wakeup_event.type == EVENT_IPC
                && t->wakeup_key == key) {
                t->wakeup_time = 0;
                t->status = TASK_READY;
            }
        }

        spinlock_release(&tasks_lock[c]);
    }
}

task_t *sched_get_current_task()
{
    cpu_t *cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return NULL;
    }

    return tasks_running[cpu->cpu_id];
}

uint64_t sched_get_ticks()
{
    cpu_t *cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return 0;
    }

    return tasks_coordinate[cpu->cpu_id];
}

void sched_init(const char *name, uint16_t cpu_id)
{
    tasks_idle[cpu_id] = task_make(name, task_idle_proc, 255,
                                   TASK_KERNEL_MODE, NULL);

    klogi("SCHED: create idle task 0x%016lx with tid %ld for CPU %ld\n",
        tasks_idle[cpu_id], tasks_idle[cpu_id]->tid, cpu_id);

    vec_push_back(&tasks_active_table[cpu_id], tasks_idle[cpu_id]);

    apic_timer_init(cpu_id);
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);

    cpu_num++;

    klogi
        ("SCHED: initialization finished for CPU %ld with idle task %s:%ld\n",
         cpu_id, name, tasks_idle[cpu_id]->tid);
}

uint16_t sched_get_cpu_num()
{
    return cpu_num;
}

task_t *sched_new(const char *name, void (*entry)(task_id_t),
                  bool usermode)
{
    task_t *t = task_make(name, entry, 0,
                          usermode ? TASK_USER_MODE : TASK_KERNEL_MODE,
                          NULL);

    return t;
}

void sched_add(task_t *t)
{
    uint16_t target = sched_pick_cpu();

    spinlock_acquire(&tasks_lock[target]);
    vec_push_back(&tasks_active_table[target], t);
    spinlock_release(&tasks_lock[target]);

    klogi("SCHED: CPU %ld dispatches tid %ld to CPU %ld\n",
          smp_get_current_cpu_id(), t->tid, target);
}

task_t *sched_execve(const char *path, const char *argv[],
                     const char *envp[], const char *cwd)
{
    int64_t i;

    klogi("SCHED: execute \"%s\" in \"%s\" directory\n", path, cwd);

    auxval_t aux = { 0 };
    uint64_t entry = 0;

    task_t *tp = sched_get_current_task();
    task_t *tc = NULL;

    char *tname = (char *) path;
    for (i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            tname = (char *) &(path[i + 1]);
            break;
        }
    }

    tc = task_make(tname, NULL, 0, TASK_USER_MODE,
                   tp == NULL ? NULL : tp->addrspace);

    if (tp != NULL) {
        uint64_t i;

        for (i = 0; i < vec_length(&tp->dup_list); i++) {
            file_dup_t dup = vec_at(&tp->dup_list, i);
            vec_push_back(&tc->dup_list, dup);
            klogd("SCHED: fh pair for tid %ld's child task %ld - (%ld, %ld)\n",
                  tp->tid, tc->tid, dup.fh, dup.newfh);
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
            klogd("SCHED: copy fd %ld from tid %ld to tid %ld\n",
                  tc->open_files_table.array[i].key, tp->tid, tc->tid);
        }
    }

    if (elf_load(tc, path, &entry, &aux)) {
        /* Need to release memory for task "tc" */
        task_free(tc);
        return NULL;
    }

    task_regs_t *tc_regs = (task_regs_t *) PHYS_TO_VIRT(tc->tstack_top);

    /* TODO: Do not check whether aux.entry == entry any more */
    uint64_t *stack = (uint64_t *) PHYS_TO_VIRT(tc->tstack_top);

    if (cwd != NULL)
        strcpy(tc->cwd, cwd);

    uint8_t *sa = (uint8_t *) tc->tstack_top;
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
        ("SCHED: tid %ld aux stack 0x%016lx (RSP 0x%016lx), entry 0x%016lx, phdr 0x%016lx, "
         "phentsize %ld, phnum %ld\n", tc->tid, stack, tc_regs->rsp,
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

    stack = (uint64_t *) ((uint64_t) stack - sizeof(task_regs_t));
    memcpy(stack, tc_regs, sizeof(task_regs_t));

    tc->tstack_top = (void *) VIRT_TO_PHYS(stack);
    tc_regs = (task_regs_t *) stack;
    tc_regs->rsp = (uint64_t) tc->tstack_top + sizeof(task_regs_t);

    klogd("SCHED: task stack top 0x%016lx, rsp 0x%016lx, top argc %ld\n",
          tc->tstack_top, tc_regs->rsp,
          *((uint64_t *) PHYS_TO_VIRT(tc_regs->rsp)));

    /* --- Stack filling finished --- */

    tc_regs->rip = (uint64_t) entry;

    klogd("SCHED: finished initialization with entry 0x%016lx\n", entry);

    if (tp != NULL) {
        klogi("SCHED: child tid %ld and parent tid %ld\n", tc->tid, tp->tid);
        spinlock_acquire(&tp->child_lock);
        vec_push_back(&tp->child_list, tc->tid);
        spinlock_release(&tp->child_lock);
        tc->ptid = tp->tid;
    }

    sched_add(tc);

    return tc;
}

/* Try to reap a task that has already exited.
 *
 * Returns 1 and stores the task's exit status in *status when a dead task was
 * found, removed from its run queue and freed; 0 when the task still exists but
 * is alive; -1 when no task with that tid can be found (it was already reaped
 * by another core's idle task).
 */
int sched_reap(task_id_t tid, int64_t *status)
{
    /* Use the effective status: a TASK_DYING task that has no live children is
     * reported as TASK_DEAD, so an exec wrapper whose replacement has exited
     * can be reaped even if the idle task never finalized it. */
    task_status_t st = sched_get_task_status(tid);

    if (st == TASK_UNKNOWN)
        return -1;              /* already reaped */
    if (st != TASK_DEAD)
        return 0;               /* still alive */

    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (tasks_idle[c] == NULL && tasks_running[c] == NULL)
            continue;

        spinlock_acquire(&tasks_lock[c]);

        task_t *rt = tasks_running[c];
        if (rt != NULL && rt->tid == tid) {
            /* Still running (possibly about to finalize its own exit). */
            spinlock_release(&tasks_lock[c]);
            return 0;
        }

        for (uint64_t i = 0; i < vec_length(&tasks_active_table[c]); i++) {
            task_t *t = vec_at(&tasks_active_table[c], i);
            if (t == NULL || t->tid != tid)
                continue;

            bool dead = (t->status == TASK_DEAD || t->status == TASK_DYING);
            int64_t exit_status = t->exit_status;

            if (!dead) {
                spinlock_release(&tasks_lock[c]);
                return 0;
            }

            vec_erase(&tasks_active_table[c], i);
            spinlock_release(&tasks_lock[c]);

            klogi("SCHED: CPU %ld reaps dead task #%ld from CPU %ld\n",
                  smp_get_current_cpu_id(), tid, c);
            if (status != NULL)
                *status = exit_status;
            task_free(t);
            return 1;
        }

        spinlock_release(&tasks_lock[c]);
    }

    return -1;
}

