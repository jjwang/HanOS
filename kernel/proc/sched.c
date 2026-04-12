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
static lock_t tasks_lock[CPU_MAX] = {0};

static volatile uint16_t cpu_num = 0;

typedef vec_struct(task_t*) task_vector_t;
task_vector_t tasks_active_table[CPU_MAX] = {0};

extern void enter_context_switch(void *v);
extern void exit_context_switch(task_t * next, uint64_t cr3val);
extern void force_context_switch(void);
extern void fork_context_switch(void);

extern addrspace_t kaddrspace;

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
        lock_lock(&(tasks_lock[cpu_id]));
        /* 1. Free resouces of dead tasks in idle task */
        task_t *t = NULL;

        /* Step 1.1: Find a dead task */
        uint64_t task_num = vec_length(&tasks_active_table[cpu_id]);
        uint64_t i;
        if (task_num > 0) {
            for (i = 0; i < task_num; i++) {
                t = vec_at(&tasks_active_table[cpu_id], i);
                if (t->status == TASK_DEAD) {
                    vec_erase(&tasks_active_table[cpu_id], i);
                    break;
                } else {
                    t = NULL;
                }
            }
        }
        if (t != NULL) {
            for (i = 0; i < task_num; i++) {
                task_t *tp = vec_at(&tasks_active_table[cpu_id], i);
                if (t->ptid == tp->tid) {
                    for (uint64_t k = 0; k < vec_length(&tp->child_list);
                         k++) {
                        task_id_t tid_child = vec_at(&tp->child_list, k);
                        if (tid_child == t->tid) {
                            vec_erase(&tp->child_list, k);
                            if (vec_length(&tp->child_list) == 0
                                && tp->status == TASK_DYING) {
                                tp->status = TASK_DEAD;
                            }
                            break;
                        }
                    }
                    break;
                }
            }
        }

        if (t != NULL) {
            klogi("sched: clean memory of dead task #%ld (0x%016lx)\n", t->tid,
                  t);

            /* Step 1.2: Free all resources of this dead task */
            task_free(t);
            lock_release(&(tasks_lock[cpu_id]));
        } else {
            lock_release(&(tasks_lock[cpu_id]));
            /* If we cannot find dead tasks, then fall into sleep */
            asm volatile ("hlt");
        }
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

    lock_lock(&(tasks_lock[cpu_id]));

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

    for (int64_t i = 0; i < tasks_num; i++) {
        task_t *t = vec_at(&tasks_active_table[cpu_id], i);
        if (t->status == TASK_READY) {
            next = t;
            vec_erase(&tasks_active_table[cpu_id], i);
            break;
        }
        if (t->status == TASK_SLEEPING) {
            if ((hpet_get_nanos() >= t->wakeup_time) && (t->wakeup_time > 0)) {
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

    lock_release(&(tasks_lock[cpu_id]));

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
    if (cpu == NULL) {
        return TID_MAX;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    task_id_t tid = TID_MAX;
    if (curr) {
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
        tid = curr->tid;
    }

    fork_context_switch();

    return tid;
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

static task_status_t sched_get_task_status_impl(task_id_t tid)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    ASSERT (cpu != NULL);

    uint16_t cpu_id = cpu->cpu_id;

    task_t *ntask = NULL;
    task_status_t status = TASK_UNKNOWN;
    bool has_child = false;

    uint64_t i;
    for (i = 0; i < vec_length(&tasks_active_table[cpu_id]); i++) {
        task_t *t = vec_at(&tasks_active_table[cpu_id], i);
        if (t) {
            if (t->tid == tid) {
                status = t->status;
                ntask = t;
            }
            if (t->ptid == tid) {
                if (t->status != TASK_DEAD && t->status != TASK_UNKNOWN) {
                    has_child = true;
                } else if (sched_get_task_status_impl(t->tid) ==
                           TASK_RUNNING) {
                    has_child = true;
                }
            }
        }
    }
    for (i = 0; i < CPU_MAX && !has_child; i++) {
        task_t *t = tasks_running[i];
        if (t) {
            if (t->tid == tid) {
                status = t->status;
                ntask = t;
            }
            if (t->ptid == tid) {
                if (t->status != TASK_DEAD && t->status != TASK_UNKNOWN) {
                    has_child = true;
                } else if (sched_get_task_status_impl(t->tid) ==
                           TASK_RUNNING) {
                    has_child = true;
                }
            }
        }
    }

    if (!has_child) {
        if (ntask != NULL) {
            if (ntask->status == TASK_DEAD || ntask->status == TASK_DYING) {
                status = TASK_UNKNOWN;
            }
        }
    } else {
        status = TASK_RUNNING;
    }

    return status;
}

task_status_t sched_get_task_status(task_id_t tid)
{
    task_status_t status = TASK_UNKNOWN;

    status = sched_get_task_status_impl(tid);

    return status;
}

void sched_exit(int64_t status)
{
    (void) status;

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    if (curr) {
        curr->status = TASK_DYING;
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
        uint64_t len = vec_length(&(curr->child_list));
        bool all_children_dead = true;
        for (uint64_t i = 0; i < len; i++) {
            task_id_t tid_child = vec_at(&(curr->child_list), i);
            task_status_t status_child =
                sched_get_task_status_impl(tid_child);
            if (status_child != TASK_DEAD) {
                all_children_dead = false;
                break;
            }
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
    }

    force_context_switch();
}

bool sched_resume_event(event_t event)
{
    cpu_t *cpu = smp_get_current_cpu(false);
    ASSERT (cpu != NULL);

    uint16_t cpu_id = cpu->cpu_id;

    bool ret = false;

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
    /* TODO: if we want to assign task to other CPUs, we need to do it carefully.
     */
    uint16_t cpu_id = smp_get_current_cpu_id();
    klogi("SCHED: CPU %ld adds tid %ld\n", cpu_id, t->tid);
    vec_push_back(&tasks_active_table[cpu_id], t);
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
        vec_push_back(&tp->child_list, tc->tid);
        tc->ptid = tp->tid;
    }

    sched_add(tc);

    return tc;
}

