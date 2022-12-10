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
#include <lib/kmalloc.h>
#include <proc/sched.h>
#include <proc/eventbus.h>
#include <core/smp.h>
#include <core/timer.h>
#include <core/apic.h>
#include <core/hpet.h>
#include <core/pit.h>
#include <core/isr_base.h>
#include <core/panic.h>

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

static lock_t sched_lock = lock_new();

static task_t* tasks_running[CPU_MAX] = {0};
static task_t* tasks_idle[CPU_MAX] = {0};
static uint64_t tasks_coordinate[CPU_MAX] = {0};

static volatile uint16_t cpu_num = 0;

vec_new_static(task_t*, tasks_active);

extern void enter_context_switch(void* v);
extern void exit_context_switch(task_t* next, uint64_t cr3val);
extern void force_context_switch();

void task_debug(void)
{
    lock_lock(&sched_lock);

    size_t task_num = vec_length(&tasks_active);
    for (size_t i = 0; i < task_num; i++) {
        task_t *t  = vec_at(&tasks_active, i);
        (void)t;
    }
    for (size_t k = 0; k < CPU_MAX; k++) {
        if (tasks_running[k] != NULL && tasks_running[k] != tasks_idle[k]) {
            task_num++;
        }
    }
    lock_release(&sched_lock);

    kprintf("Totally %d active tasks.\n", task_num);
}

_Noreturn static void task_idle_proc(task_id_t tid)
{
    (void)tid;

    while (true) {
        asm volatile ("hlt");
    }
}

/*
 * force parameter will be set to true when called from
 * enter_force_context_switch()
 *
 */
void do_context_switch(void* stack, int64_t force)
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

    task_t* curr = tasks_running[cpu_id];
    task_t* next = NULL;

    if (curr) {
        curr->tstack_top = stack;
        curr->last_tick = ticks;

        if (curr->status == TASK_RUNNING)
            curr->status = TASK_READY;

        if ((uint64_t)curr != (uint64_t)tasks_idle[cpu_id]) {
            vec_push_back(&tasks_active, curr);
        }

    }

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

    /* Need to review TSS related settings */
    cpu->tss.rsp0 = (uint64_t)(next->kstack_limit + STACK_SIZE);

    tasks_coordinate[cpu_id]++;
    
    if (!force) {
        apic_send_eoi();
    }

    lock_release(&sched_lock);

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
    }

    lock_release(&sched_lock);

    force_context_switch();
}

void sched_resume_event(event_t event)
{
    lock_lock(&sched_lock);
    for (size_t i = 0; i < vec_length(&tasks_active); i++) {
        task_t *t = vec_at(&tasks_active, 0);
        if (t) {
            if (t->status == TASK_SLEEPING
                && t->wakeup_event.type == event.type) {
                t->status = TASK_READY;
                t->wakeup_event.para = event.para;
            }
        }
    }
    lock_release(&sched_lock);
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
    tasks_idle[cpu_id] = task_make(__func__, task_idle_proc, 255, TASK_KERNEL_MODE);

    apic_timer_init(); 
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);
    apic_timer_start();

    cpu_num++;

    klogi("Scheduler initialization finished for CPU %d\n", cpu_id);
}

uint16_t sched_get_cpu_num()
{
    return cpu_num;
}

task_t *sched_add(void (*entry)(task_id_t), bool usermode)
{
    task_t* t = task_make(__func__, entry, 0,
        usermode ? TASK_USER_MODE : TASK_KERNEL_MODE);

    lock_lock(&sched_lock);
    vec_push_back(&tasks_active, t);
    lock_release(&sched_lock);

    return t;
}

