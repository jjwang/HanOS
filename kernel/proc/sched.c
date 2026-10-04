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
#include <stdint.h>
#include <string.h>

#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/time.h>
#include <lib/kmalloc.h>
#include <lib/vector.h>
#include <proc/sched.h>
#include <proc/elf.h>
#include <arch/x64/smp.h>
#include <arch/x64/timer.h>
#include <arch/x64/apic.h>
#include <arch/x64/hpet.h>
#include <arch/x64/pit.h>
#include <arch/x64/isr_base.h>
#include <arch/x64/idt.h>
#include <arch/x64/panic.h>
#include <router/router.h>
#include <arch/x64/cpu.h>
#include <arch/x64/serial.h>
#include <srv/process_srv.h>
#include <printf.h>

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

/* Per-CPU scheduler state. Each core owns a run queue holding the processes
 * assigned to it (ready, sleeping or dying) plus the process currently running.
 * Global pid lookup goes through the process table in process.c. */
static process_t *running_process[CPU_MAX] = { 0 };
static process_t *idle_process[CPU_MAX] = { 0 };
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
        for (uint16_t i = 0; i < info->num_cpus && i < CPU_MAX; i++) {
            uint16_t id = info->cpus[i].cpu_id;

            /* Only dispatch to a core whose scheduler has created its idle
             * task. Before that, an IPI would reach enter_context_switch with
             * running_process[] and idle_process[] still NULL. */
            if (idle_process[id] != NULL)
                ids[n++] = id;
        }
    }
    if (n == 0)
        ids[n++] = smp_get_current_cpu_id();

    static uint32_t rr_index;
    uint16_t idx =
        (uint16_t) (__atomic_fetch_add(&rr_index, 1, __ATOMIC_RELAXED) % n);

    return ids[idx];
}

_Noreturn void process_idle(pid_t pid)
{
    /* TODO: Need to find out why a #PF occurs without sleeping. */
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
        uint64_t queue_len = vec_length(&run_queues[cpu_id]);
        for (uint64_t i = 0; i < queue_len; i++) {
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

        klogi("sched: clean memory of dead process #%ld (0x%016lx)\n", t->pid, t);

        /* A dead server owner drops its endpoint; the monitor restarts it. */
        router_owner_died(t->pid);

        /* Step 2: Free all resources of this dead process */
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

    process_t *curr = running_process[cpu_id];

    if (curr != NULL) {
        /* Process status: 0 - ready, 1 - running, 2 - sleeping */
        curr->context = stack;
        curr->last_tick = ticks;
        curr->errno = cpu->errno;

        if ((uint64_t) curr != (uint64_t) idle_process[cpu_id]) {
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
                running_process[cpu_id] = NULL;
                curr = NULL;
            }
        } else {
            curr = NULL;
        }
    }

    process_t *next = NULL;
    int64_t queue_len = vec_length(&run_queues[cpu_id]);

    /* Promote sleepers whose timer has expired to ready, so they compete for
     * the core like any other runnable process. Picking an expired sleeper only
     * when nothing else is ready lets a perpetually runnable process (a server
     * polling loop, the UI thread) starve timed sleepers indefinitely. */
    for (int64_t i = 0; i < queue_len; i++) {
        process_t *t = vec_at(&run_queues[cpu_id], i);
        if (t->status == PROC_SLEEPING
            && t->wakeup_time > 0
            && hpet_get_nanos() >= t->wakeup_time) {
            t->status = PROC_READY;
            t->wakeup_time = 0;
        }
    }

    for (int64_t i = 0; i < queue_len; i++) {
        process_t *t = vec_at(&run_queues[cpu_id], i);
        if (t->status == PROC_READY) {
            next = t;
            vec_erase(&run_queues[cpu_id], i);
            break;
        }
    }

    if (next != NULL) {
        if (curr != NULL) {
            curr->status = PROC_READY;
            vec_push_back(&run_queues[cpu_id], curr);
        }
    } else {
        next = (curr == NULL) ? idle_process[cpu_id] : curr;
    }

    next->status = PROC_RUNNING;
    running_process[cpu_id] = next;

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
    process_t *curr = running_process[cpu_id];
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
    if (running_process[cpu_id] && running_process[cpu_id]->pid < 1)
        kpanic("SCHED: %s meets corrupted pid\n", __func__);
    fork_context_switch();
    return running_process[cpu_id]->fork_retval;
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
    process_t *curr = running_process[cpu_id];
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

/* Wake up a parent that is sleeping in waitpid() for one of its children to
 * exit. The parent re-checks with the process server after waking, so it is
 * enough to mark every sleeper that waits on EVENT_CHILD_EXIT as ready.
 */
static void sched_wake_child_waiter(pid_t parent_pid)
{
    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_process[c] == NULL && running_process[c] == NULL)
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
    (void) status;

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return;
    }

    /* Keep interrupts disabled until the final context switch: a process that
     * is switched out while dead is never selected again. The process server
     * holds the exit status and does the parent/child bookkeeping. */
    asm volatile ("cli" ::: "memory");

    uint16_t cpu_id = cpu->cpu_id;
    process_t *curr = running_process[cpu_id];
    if (curr) {
        if (curr->pid < 1) {
            kpanic("SCHED: %s meets corrupted pid\n", __func__);
        }
        curr->status = PROC_DEAD;
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
    process_t *curr = running_process[cpu_id];
    if (curr == NULL) {
        return;
    }

    curr->wakeup_event.type = EVENT_CHILD_EXIT;
    curr->wakeup_event.para = 0;
    curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
    curr->status = PROC_SLEEPING;

    force_context_switch();
}

/* Arm the current process to be woken by sched_wake_key(key). This must run
 * before the caller checks whether it has anything to wait for, so that a wake
 * delivered in that window can be observed by sched_wait_key_commit(). */
void sched_wait_key_begin(void *key)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL)
        return;

    uint16_t cpu_id = cpu->cpu_id;
    spinlock_acquire(&run_queue_lock[cpu_id]);

    process_t *curr = running_process[cpu_id];
    if (curr != NULL) {
        curr->wakeup_event.type = EVENT_IPC;
        curr->wakeup_event.para = 0;
        curr->wakeup_key = key;
        curr->wakeup_pending = false;
    }

    spinlock_release(&run_queue_lock[cpu_id]);
}

/* Stop waiting on the armed key and drop any wake that arrived meanwhile. */
void sched_wait_key_cancel(void)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL)
        return;

    uint16_t cpu_id = cpu->cpu_id;
    spinlock_acquire(&run_queue_lock[cpu_id]);

    process_t *curr = running_process[cpu_id];
    if (curr != NULL) {
        curr->wakeup_key = NULL;
        curr->wakeup_pending = false;
    }

    spinlock_release(&run_queue_lock[cpu_id]);
}

/* Block the current process until sched_wake_key(key) is called or the timeout
 * expires. Used by the IPC receive path. If a wake already arrived after
 * sched_wait_key_begin(), consume it and return immediately so the caller can
 * re-check its queue; otherwise park. */
void sched_wait_key_commit(time_t millis)
{
    uint64_t deadline = hpet_get_nanos() + MILLIS_TO_NANOS(millis);

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    spinlock_acquire(&run_queue_lock[cpu_id]);

    process_t *curr = running_process[cpu_id];
    if (curr == NULL) {
        spinlock_release(&run_queue_lock[cpu_id]);
        return;
    }

    if (curr->wakeup_pending) {
        curr->wakeup_pending = false;
        spinlock_release(&run_queue_lock[cpu_id]);
        return;
    }

    curr->wakeup_time = deadline;
    curr->status = PROC_SLEEPING;

    spinlock_release(&run_queue_lock[cpu_id]);

    force_context_switch();
}

/* Wake a process that is armed on the given key. A process that has not parked
 * yet is marked pending instead, so it observes the wake before it sleeps.
 * Returns true when the process became runnable. */
static bool sched_wake_one(process_t *t, void *key)
{
    if (t == NULL || t->wakeup_event.type != EVENT_IPC
        || t->wakeup_key != key)
        return false;

    if (t->status == PROC_SLEEPING) {
        t->wakeup_time = 0;
        t->status = PROC_READY;
    } else {
        t->wakeup_pending = true;
    }

    return true;
}

/* Block the current process on the armed key with no timeout. */
void sched_wait_key_commit_infinite(void)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL)
        return;

    uint16_t cpu_id = cpu->cpu_id;
    spinlock_acquire(&run_queue_lock[cpu_id]);

    process_t *curr = running_process[cpu_id];
    if (curr == NULL) {
        spinlock_release(&run_queue_lock[cpu_id]);
        return;
    }

    if (curr->wakeup_pending) {
        curr->wakeup_pending = false;
        spinlock_release(&run_queue_lock[cpu_id]);
        return;
    }

    curr->wakeup_time = 0;
    curr->status = PROC_SLEEPING;

    spinlock_release(&run_queue_lock[cpu_id]);

    force_context_switch();
}

/* Wake up to n processes sleeping on the given key, on any core. Returns the
 * number woken. A process armed but not yet parked consumes a slot through its
 * pending flag. */
int64_t sched_wake_key_n(void *key, int64_t n)
{
    if (n <= 0)
        return 0;

    int64_t woken = 0;
    uint16_t cur = smp_get_current_cpu_id();

    for (uint16_t c = 0; c < CPU_MAX && n > 0; c++) {
        if (idle_process[c] == NULL && running_process[c] == NULL)
            continue;

        bool woke = false;

        spinlock_acquire(&run_queue_lock[c]);

        if (sched_wake_one(running_process[c], key)) {
            woke = true;
            woken++;
            n--;
        }

        uint64_t len = vec_length(&run_queues[c]);

        for (uint64_t i = 0; i < len && n > 0; i++) {
            if (sched_wake_one(vec_at(&run_queues[c], i), key)) {
                woke = true;
                woken++;
                n--;
            }
        }

        spinlock_release(&run_queue_lock[c]);

        /* Kick the core that owns the woken process, so it reschedules now
         * instead of waiting for its own timer tick. */
        if (woke && c != cur && sched_ipi_vector != 0)
            apic_send_ipi(c, sched_ipi_vector, 0);
    }

    return woken;
}

/* Wake every process sleeping on the given key, on any core. */
void sched_wake_key(void *key)
{
    sched_wake_key_n(key, INT64_MAX);
}

/* Mark every thread of the group dead, except the caller. The idle reaper
 * frees them; a running thread is reaped after its next context switch. */
void sched_kill_group(pid_t tgid, pid_t except)
{
    uint16_t cur = smp_get_current_cpu_id();

    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_process[c] == NULL && running_process[c] == NULL)
            continue;

        bool woke = false;

        spinlock_acquire(&run_queue_lock[c]);

        process_t *rp = running_process[c];
        if (rp != NULL && rp->tgid == tgid && rp->pid != except
            && rp->status != PROC_DEAD) {
            rp->status = PROC_DEAD;
            woke = true;
        }

        uint64_t n = vec_length(&run_queues[c]);
        for (uint64_t i = 0; i < n; i++) {
            process_t *p = vec_at(&run_queues[c], i);
            if (p != NULL && p->tgid == tgid && p->pid != except
                && p->status != PROC_DEAD) {
                p->status = PROC_DEAD;
                woke = true;
            }
        }

        spinlock_release(&run_queue_lock[c]);

        if (woke && c != cur && sched_ipi_vector != 0)
            apic_send_ipi(c, sched_ipi_vector, 0);
    }
}

/* Make a sleeping process runnable wherever it is parked. A process armed but
 * not yet parked is left alone: it will observe the wake after it sleeps. */
void sched_wake_process(process_t * t)
{
    if (t == NULL)
        return;

    uint16_t cur = smp_get_current_cpu_id();

    for (uint16_t c = 0; c < CPU_MAX; c++) {
        if (idle_process[c] == NULL && running_process[c] == NULL)
            continue;

        bool woke = false;

        spinlock_acquire(&run_queue_lock[c]);

        if (running_process[c] == t && t->status == PROC_SLEEPING) {
            t->wakeup_time = 0;
            t->status = PROC_READY;
            woke = true;
        }

        uint64_t n = vec_length(&run_queues[c]);
        for (uint64_t i = 0; i < n; i++) {
            process_t *p = vec_at(&run_queues[c], i);

            if (p == t && p->status == PROC_SLEEPING) {
                p->wakeup_time = 0;
                p->status = PROC_READY;
                woke = true;
            }
        }

        spinlock_release(&run_queue_lock[c]);

        if (woke && c != cur && sched_ipi_vector != 0)
            apic_send_ipi(c, sched_ipi_vector, 0);
    }
}

/* The process server clones a child's fd table after fork. The parent marks
 * the child ready once the server replies; the child waits here, so it never
 * runs with an incomplete fd table. */
void sched_mark_fds_ready(pid_t pid)
{
    process_t *t = process_lookup(pid);
    if (t == NULL)
        return;

    t->fds_ready = true;
    sched_wake_key(t);
}

void sched_wait_fds_ready(process_t * t)
{
    for (;;) {
        if (t->fds_ready)
            return;

        sched_wait_key_begin(t);
        if (t->fds_ready) {
            sched_wait_key_cancel();
            return;
        }
        sched_wait_key_commit(1000);
    }
}

process_t *sched_get_current_process()
{
    cpu_t *cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return NULL;
    }

    return running_process[cpu->cpu_id];
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
    idle_process[cpu_id] = process_make(name, process_idle, 255,
                                   PROC_KERNEL_MODE, NULL);

    klogi("SCHED: create idle process 0x%016lx with pid %ld for CPU %ld\n",
        idle_process[cpu_id], idle_process[cpu_id]->pid, cpu_id);

    /* The idle process is deliberately kept out of the run queue: it is the
     * scheduler's fallback (see schedule_next) and must never be selected as a
     * preemption target. Queuing it here would let an IPI arriving before this
     * core's APIC timer is started park the boot thread in the idle loop, which
     * could then never be woken because its timer was never armed. */

    uint8_t timer_vector = apic_timer_init(cpu_id);

    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);

    /* Install the context-switch entry on this core's own timer vector. The
     * vector is per-CPU, so binding it through the shared global in timer.c
     * would let a concurrently initialising core install the handler on the
     * wrong vector, leaving this core's timer on the EOI-only handler. */
    idt_set_handler(timer_vector, enter_context_switch);

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
         cpu_id, name, idle_process[cpu_id]->pid);
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

    if (elf_load(tc, path, &entry, &aux)) {
        /* Need to release memory for process "tc" */
        process_free(tc);
        return NULL;
    }

    /* The process server clones the parent's fd table for the new process;
     * this kernel path is a fork+exec. Register only after the image loaded,
     * or a failed exec leaves a live child the parent waits on forever. */
    if (tp != NULL) {
        process_fd_fork((int32_t) tp->pid, (int32_t) tc->pid);
    }

    process_regs_t *tc_regs = (process_regs_t *) PHYS_TO_VIRT(tc->context);

    /* TODO: Do not check whether aux.entry == entry any more */
    uint64_t *stack = (uint64_t *) PHYS_TO_VIRT(tc->context);

    if (cwd != NULL)
        process_set_cwd(tc, cwd);

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
        tc->ppid = tp->pid;
    }

    if (sched_spawn_hook != NULL)
        sched_spawn_hook(tc);

    sched_add(tc);

    return tc;
}

