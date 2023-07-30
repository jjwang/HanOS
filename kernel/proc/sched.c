/**-----------------------------------------------------------------------------

 @file    sched.c
 @brief   Implementation of scheduling related functions
 @details
 @verbatim

  Context Switching, Scheduling Algorithms etc.

  History:
  Apr 20, 2022 - 1. Redesign the task queue based on vector data structue.
                 2. Scheduler starts working after all processors are launched
                    to avoid GPF exception.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <lib/klog.h>
#include <lib/lock.h>
#include <lib/time.h>
#include <lib/string.h>
#include <lib/kmalloc.h>
#include <lib/vector.h>
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

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

lock_t sched_lock = lock_new();

static task_t* tasks_running[CPU_MAX] = {0};
static task_t* tasks_idle[CPU_MAX] = {0};
static uint64_t tasks_coordinate[CPU_MAX] = {0};

static volatile uint16_t cpu_num = 0;

vec_new_static(task_t*, tasks_active);

extern void enter_context_switch(void* v);
extern void exit_context_switch(task_t* next, uint64_t cr3val);
extern void force_context_switch(void);
extern void fork_context_switch(void);

void sched_debug(bool showlog)
{
    lock_lock(&sched_lock);

    size_t task_num = vec_length(&tasks_active);

    if (showlog)
        klogd("SCHED: Totally %d active tasks\n", task_num);

    for (size_t i = 0; i < task_num; i++) {
        task_t *t  = vec_at(&tasks_active, i);
        if (t->tid < 1)
            kpanic("SCHED: task list corrupted (%d 0x%x)\n", showlog, t);
    }

    for (size_t k = 0; k < CPU_MAX; k++) {
        if (tasks_running[k] != NULL && tasks_running[k] != tasks_idle[k]) {
            if (showlog) {
                klogd("SCHED: CPU %d has running task "
                      "(kernel 0x%x|0x%x user 0x%x|%x in tid %d)\n",
                      k, tasks_running[k]->kstack_top, tasks_running[k]->kstack_limit,
                      tasks_running[k]->ustack_top, tasks_running[k]->ustack_limit,
                      tasks_running[k]->tid);
            }
            task_num++;
            if (tasks_running[k]->tid < 1) {
                kpanic("SCHED: running task on CPU %d corrupted (%d 0x%x)\n",
                       k, showlog, tasks_running[k]);
            }
        }

        if (tasks_idle[k]->tid < 1) {
            kpanic("SCHED: idle task on CPU %d corrupted (%d 0x%x)\n",
                k, showlog, tasks_idle[k]);
        }

    }

    lock_release(&sched_lock);
}

_Noreturn static void task_idle_proc(task_id_t tid)
{
    (void)tid;

    while (true) {
        /* 1. Free resouces of dead tasks in idle task */
        task_t *t = NULL;

        /* Step 1.1: Find a dead task */
        lock_lock(&sched_lock);
        size_t task_num = vec_length(&tasks_active);
        if (task_num > 0) {
            for (size_t i = 0; i < task_num; i++) {
                t = vec_at(&tasks_active, i);
                if (t->status == TASK_DEAD) {
                    vec_erase(&tasks_active, i);
                    break;
                } else {
                    t = NULL;
                }
            }
        }
        lock_release(&sched_lock);

        if (t != NULL) {
            klogi("sched: clean memory of dead task #%d\n", t->tid);

            /* Step 1.2: Free all resources of this dead task */
            size_t mmap_num = vec_length(&t->mmap_list);
            for (size_t i = 0; i < mmap_num; i++) {
                mem_map_t m = vec_at(&t->mmap_list, i); 
                vmm_unmap(t->addrspace, m.vaddr, m.np, false);
                kmfree((void*)PHYS_TO_VIRT(m.paddr));
            }  
            vec_erase_all(&t->mmap_list);

            klogi("task_idle: dead task tid %d free mmap number %d\n",
                  t->tid, mmap_num);

            /* 2. Free memory when creating a new task */
            if (t->mode == TASK_USER_MODE) {
                /* Notes that ustack memory is already free in mmap_list */
            }
            kmfree((void*)t->kstack_limit);

            size_t mem_num = vec_length(&t->addrspace->mem_list);
            for (size_t i = 0; i < mem_num; i++) {
                /*
                 * Maybe it was already freed in unmap(), but it is also
                 * OK freed here because pmm_free will not crash.
                 */
                uint64_t m = vec_at(&t->addrspace->mem_list, i); 
                pmm_free(m, 8, __func__, __LINE__);
            }   
            vec_erase_all(&t->addrspace->mem_list);

            kmfree((void*)t->addrspace->PML4);
            kmfree((void*)t->addrspace);
            kmfree(t);
        } else {
            /* If we cannot find dead tasks, then fall into sleep */
            asm volatile ("hlt");
        }
    }
}

/*
 * Context switch has 3 situations which are determined by parameter "mode":
 * [0]: triggered by timer cycle.
 * [1]: triggered by task itself which needs to fall in sleep.
 * [2]: triggered by fork which needs to create a clone.
 *
 */
void do_context_switch(void* stack, int64_t mode)
{
    const smp_info_t* smp_info = smp_get_info();
    if (smp_info == NULL)               return;

    /* Make sure that all CPUs initialization finished */
    if (smp_info->num_cpus != cpu_num)  return;

    /* Firstly all events on event bus should be processed */
    eb_dispatch();

    lock_lock(&sched_lock);

    cpu_t *cpu = smp_get_current_cpu(true);
    if (cpu == NULL) {
        lock_release(&sched_lock);
        return;
    }

    uint16_t cpu_id = cpu->cpu_id;
    uint64_t ticks = tasks_coordinate[cpu_id];

    task_t *curr = tasks_running[cpu_id];
    task_t *next = NULL;

    if (curr) {
        curr->tstack_top = stack;
        curr->last_tick = ticks;
        curr->errno = cpu->errno;

        if (curr->status == TASK_RUNNING)
            curr->status = TASK_READY;

        if ((uint64_t)curr != (uint64_t)tasks_idle[cpu_id]) {
            if (mode == 2) {
                task_t *curr_fork = task_fork(curr);
                vec_push_back(&tasks_active, curr_fork);
            }
            vec_push_back(&tasks_active, curr);
        }

    }
    tasks_running[cpu_id] = NULL;
    curr = NULL;

    uint64_t loop_size = 0;
    while (true) {
        if (vec_length(&tasks_active) > 0) {
            next = vec_at(&tasks_active, 0);
            vec_erase(&tasks_active, 0);
        } else {
            next = NULL;
            break;
        }
        if (next->status == TASK_READY) break;
        if (next->status == TASK_SLEEPING) {
            if ((hpet_get_nanos() >= next->wakeup_time)
                && (next->wakeup_time > 0)) {
                break;
            }
        }
        vec_push_back(&tasks_active, next);
        /* If the whole task list is visited, exit with NULL task */
        loop_size++;
        if (loop_size >= vec_length(&tasks_active)) {
            next = NULL;
            break;
        }
    }

    if (next == NULL) {
        next = tasks_idle[cpu_id];
    }

    next->status = TASK_RUNNING;
    tasks_running[cpu_id] = next;

    cpu->errno = next->errno;
    cpu->tss.rsp0 = (uint64_t)(next->kstack_limit + STACK_SIZE);

    tasks_coordinate[cpu_id]++;
    
    if (mode == 0) {
        apic_send_eoi();
    }

    lock_release(&sched_lock);

    if (!(cpu->tss.rsp0 & 0xFFFF000000000000) || next->tid < 1) {
        sched_debug(true);
        kpanic("SCHED: CPU %d kernel stack 0x%x corrputed "
               "(kernel 0x%x|0x%x user 0x%x|%x in task 0x%x tid %d, last tick %d)\n",
               cpu->cpu_id, cpu->tss.rsp0, next->kstack_top, next->kstack_limit,
               next->ustack_top, next->ustack_limit, next, next->tid, next->last_tick);
    }

    if (next->fs_base != 0
        && read_msr(MSR_FS_BASE) != next->fs_base)
    {
        /*
         * Here we must set to the corresponding correct FS_BASE, else it will
         * bring Page Fault exception.
         */
        write_msr(MSR_FS_BASE, next->fs_base);
    }

    exit_context_switch(next->tstack_top,
        (next->addrspace == NULL)
            ? 0 : VIRT_TO_PHYS((uint64_t)next->addrspace->PML4));
}

task_id_t sched_get_tid()
{
    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return TID_MAX;
    }   
 
    lock_lock(&sched_lock);

    uint16_t cpu_id = cpu->cpu_id;
    task_t* curr = tasks_running[cpu_id];
    task_id_t tid = curr->tid;

    lock_release(&sched_lock);

    if (tid < 1) kpanic("SCHED: %s returns corrupted tid\n", __func__);

    return tid;
}

task_id_t sched_fork(void)
{
    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return TID_MAX;
    }   
 
    lock_lock(&sched_lock);

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    task_id_t tid = TID_MAX;
    if (curr) {
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
        tid = curr->tid;
    }

    lock_release(&sched_lock);

    fork_context_switch();

    return tid;
}

void sched_sleep(time_t millis)
{
    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        hpet_sleep(millis);
        return;
    }
 
    lock_lock(&sched_lock);

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

    lock_release(&sched_lock);

    force_context_switch();
}

task_status_t sched_get_task_status(task_id_t tid)
{
    task_t *ntask = NULL;
    task_status_t status = TASK_UNKNOWN;
    bool has_child = false; 

    lock_lock(&sched_lock);
    size_t i;
    for (i = 0; i < vec_length(&tasks_active); i++) {
        task_t *t = vec_at(&tasks_active, i); 
        if (t) {
            if (t->tid == tid) {
                status = t->status;
                ntask = t;
            }
            if (t->ptid == tid && t->status != TASK_DEAD
                && t->status != TASK_UNKNOWN)
            {
                has_child = true;
            }
        }   
    }
    for (i = 0; i < CPU_MAX; i++) {
        task_t *t = tasks_running[i];
        if (t) {
            if (t->tid == tid) {
                status = t->status;
                ntask = t;
            }
            if (t->ptid == tid && t->status != TASK_DEAD
                && t->status != TASK_UNKNOWN)
            {   
                has_child = true;
            } 
        }   
    }
    lock_release(&sched_lock);

    if (!has_child) {
        if (ntask != NULL) {
            if (ntask->status == TASK_DEAD) status = TASK_UNKNOWN;
        }
    } else {
        status = TASK_RUNNING;
    }

    return status;
}

void sched_exit(int64_t status)
{
    (void)status;

    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return;
    }   
 
    lock_lock(&sched_lock);

    uint16_t cpu_id = cpu->cpu_id;
    task_t *curr = tasks_running[cpu_id];
    if (curr) {
        curr->status = TASK_DEAD;
        if (curr->tid < 1) {
            kpanic("SCHED: %s meets corrupted tid\n", __func__);
        }
    }   

    lock_release(&sched_lock);

    force_context_switch();
}

bool sched_resume_event(event_t event)
{
    bool ret = false;

    lock_lock(&sched_lock);
    for (size_t i = 0; i < vec_length(&tasks_active); i++) {
        task_t *t = vec_at(&tasks_active, i);
        if (t) {
            if (t->status == TASK_SLEEPING
                && t->wakeup_event.type == event.type)
            {
                t->status = TASK_READY;
                t->wakeup_event.para = event.para;
                ret = true;
            }
        }
    }
    lock_release(&sched_lock);

    return ret;
}

event_t sched_wait_event(event_t event)
{
    event_t e = {0};
    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        return e;
    }   
   
    lock_lock(&sched_lock);

    uint16_t cpu_id = cpu->cpu_id;
    task_t* curr = tasks_running[cpu_id];
    curr->wakeup_time = 0;
    curr->wakeup_event = event;
    curr->status = TASK_SLEEPING;

    if (curr->tid < 1)
        kpanic("SCHED: %s meets corrupted tid\n", __func__);

    lock_release(&sched_lock);

    force_context_switch();

    return curr->wakeup_event;
}

task_t* sched_get_current_task()
{
    cpu_t* cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return NULL;
    }

    return tasks_running[cpu->cpu_id];
}

uint64_t sched_get_ticks()
{
    cpu_t* cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return 0;
    }

    return tasks_coordinate[cpu->cpu_id];
}

void sched_init(uint16_t cpu_id)
{
    lock_lock(&sched_lock);
    tasks_idle[cpu_id] = task_make(__func__, task_idle_proc, 255,
                                   TASK_KERNEL_MODE, NULL);
    lock_release(&sched_lock);

    apic_timer_init(); 
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);
    apic_timer_start();

    cpu_num++;

    klogi("Scheduler initialization finished for CPU %d with idle task %d\n",
          cpu_id, tasks_idle[cpu_id]->tid);
}

uint16_t sched_get_cpu_num()
{
    return cpu_num;
}

task_t *sched_new(void (*entry)(task_id_t), bool usermode)
{
    lock_lock(&sched_lock);
    task_t *t = task_make(
        __func__, entry, 0, usermode ? TASK_USER_MODE : TASK_KERNEL_MODE,
        NULL);
    lock_release(&sched_lock);

    return t;
}

void sched_add(task_t *t)
{
    lock_lock(&sched_lock);
    vec_push_back(&tasks_active, t); 
    lock_release(&sched_lock);
}

task_t *sched_execve(
    const char *path, const char *argv[], const char *envp[], const char *cwd)
{
    klogi("SCHED: execute \"%s\" in \"%s\" directory\n", path, cwd);

    auxval_t aux = {0};
    uint64_t entry = 0;

    task_t *tp = sched_get_current_task();
    task_t *tc = NULL;

    lock_lock(&sched_lock);
    tc = task_make(__func__, NULL, 0, TASK_USER_MODE,
                   tp == NULL ? NULL : tp->addrspace);
    lock_release(&sched_lock);

    if (elf_load(tc, path, &entry, &aux)) {
        return NULL;
    }

    task_regs_t *tc_regs = (task_regs_t*)PHYS_TO_VIRT(tc->tstack_top);

    klogd("SCHED: execve with regs 0x%x and stack top 0x%x\n",
          tc_regs, tc->tstack_top); 

    if (aux.entry != entry) {
        uint64_t *stack = (uint64_t*)PHYS_TO_VIRT(tc->tstack_top);

        if (cwd != NULL) strcpy(tc->cwd, cwd);

        uint8_t *sa = (uint8_t*)stack;
        size_t nenv = 0, nargs = 0;

        if (argv != NULL && envp != NULL) {
            for (const char **e = envp; *e; e++) {
                stack = (void*)stack - (strlen(*e) + 1);
                strcpy((char*)stack, *e);
                nenv++;
            }

            for (const char **e = argv; *e; e++) {
                stack = (void*)stack - (strlen(*e) + 1);
                strcpy((char*)stack, *e);
                nargs++;
            }

            /* Align stack address to 16-byte */
            stack = (void*)stack - ((uintptr_t)stack & 0xf);

            if ((nargs + nenv + 1) & 1)
                stack--;
        } else {
            *(--stack) = 0;
        }

        /* Auxilary vector */
        *(--stack) = 0;
        *(--stack) = 0;

        stack   -= 2;
        stack[0] = 10;  /* AT_ENTRY */
        stack[1] = aux.entry;

        stack   -= 2;
        stack[0] = 20;  /* AT_PHDR */
        stack[1] = aux.phdr;

        stack   -= 2;
        stack[0] = 21;  /* AT_PHENT */
        stack[1] = aux.phentsize;

        stack   -= 2;
        stack[0] = 22;  /* AT_PHNUM */
        stack[1] = aux.phnum;

        klogi("SCHED: tid %d aux stack 0x%x, entry 0x%x, phdr 0x%x, "
              "phentsize %d, phnum %d\n", tc->tid, stack, aux.entry,
              aux.phdr, aux.phentsize, aux.phnum);

        /* Environment variables */
        *(--stack) = 0;     /* End of environment */

        if (argv != NULL && envp != NULL) {
            stack -= nenv;
            for (size_t i = 0; i < nenv; i++) {
                sa -= strlen(envp[i]) + 1;
                stack[i] = (uint64_t)sa;
            }
        }

        /* Arguments */
        *(--stack) = 0;     /* End of arguments */

        if (argv != NULL && envp != NULL) {
            stack -= nargs;
            for (size_t i = 0; i < nargs; i++) {
                sa -= strlen(argv[i]) + 1;
                stack[i] = (uint64_t)sa;
            }
            *(--stack) = nargs; /* argc */
        } else {
            *(--stack) = 0;
        }

        stack = (uint64_t*)((uint64_t)stack - sizeof(task_regs_t));
        memcpy(stack, tc_regs, sizeof(task_regs_t));

        tc->tstack_top = (void*)VIRT_TO_PHYS(stack);
        tc_regs = (task_regs_t*)stack;
        tc_regs->rsp = (uint64_t)tc->tstack_top + sizeof(task_regs_t);
    }

    tc_regs->rip = (uint64_t)entry;

    klogd("SCHED: finished initialization with entry 0x%x\n", entry);

    lock_lock(&sched_lock);
    if (tp != NULL) {
        vec_push_back(&tp->child_list, tc->tid);
        tc->ptid = tp->tid;
    }
    lock_release(&sched_lock);

    task_debug(tc, true);

    sched_add(tc);

    return tc;
}
