/**-----------------------------------------------------------------------------

 @file    sched.c
 @brief   Implementation of scheduling related functions
 @details
 @verbatim

  Include Context Switching, Scheduling Algorithms etc.

 @endverbatim
 @author  JW
 @date    Jan 2, 2022
 @todo    Need to consider when HPET is not available (e.g., Hyper-V VM) 

 **-----------------------------------------------------------------------------
 */
#include <lib/klog.h>
#include <lib/lock.h>
#include <lib/time.h>
#include <lib/kmalloc.h>
#include <proc/sched.h>
#include <proc/tlist.h>
#include <core/smp.h>
#include <core/timer.h>
#include <core/apic.h>
#include <core/hpet.h>
#include <core/pit.h>
#include <core/isr_base.h>

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

static lock_t sched_lock = lock_new();

static task_t* tasks_running[CPU_MAX] = {0};
static task_t* tasks_idle[CPU_MAX] = {0};
static uint64_t tasks_coordinate[CPU_MAX] = {0};

static task_list_t tasks_active = {0};

extern void enter_context_switch(void* v);
extern void exit_context_switch(task_t* next);

_Noreturn static void task_idle_proc(task_id_t tid)
{
    (void)tid;

#if 0
    /* This is for writing test of user space memory. */
    uint64_t* temp_val = (uint64_t*)0x20001000;
    *temp_val = 0;
#endif

    while (true) {
        asm volatile ("nop;");
    }
}

void do_context_switch(void* stack)
{
    lock_lock(&sched_lock);

    uint16_t cpu_id = smp_get_current_cpu(true)->cpu_id;
    uint64_t ticks = tasks_coordinate[cpu_id];

    task_t* curr = tasks_running[cpu_id];
    if (curr) {
        curr->kstack_top = stack;
        curr->last_tick = ticks;

        if (curr->status == TASK_RUNNING)
            curr->status = TASK_READY;

        if ((uint64_t)curr != (uint64_t)tasks_idle[cpu_id]) {
            task_list_push(&tasks_active, curr);
        }
    }

    task_t* next = NULL;
    uint64_t loop_size = 0;
    while (true) {
        next = task_list_pop(&tasks_active);
        if (next == NULL) break;
        if (next->status == TASK_READY) break;
        if (next->status == TASK_SLEEPING) {
            if (hpet_get_nanos() >= next->wakeup_time) {
                break;
            }
        }
        task_list_push(&tasks_active, next);
        /* If the whole task list is visited, exit with NULL task */
        loop_size++;
        if (loop_size >= tasks_active.size) {
            next = NULL;
            break;
        }
    }

    if (next == NULL) {
        next = tasks_idle[cpu_id];
    }

    next->status = TASK_RUNNING;
    tasks_running[cpu_id] = next;

    smp_get_current_cpu(true)->tss.rsp0 = (uint64_t)(next->kstack_limit + KSTACK_SIZE);
    tasks_coordinate[cpu_id]++;
    
    lock_release(&sched_lock);

    apic_send_eoi();
    
    exit_context_switch(next->kstack_top);
}

void sched_sleep(time_t millis)
{
    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu == NULL) {
        pit_wait(millis);
        return;
    }
   
    lock_lock(&sched_lock);

    uint16_t cpu_id = cpu->cpu_id;
    task_t* curr = tasks_running[cpu_id];
    if (curr) {
        curr->wakeup_time = hpet_get_nanos() + MILLIS_TO_NANOS(millis);
        curr->status = TASK_SLEEPING;
    }   

    lock_release(&sched_lock);

    asm volatile("hlt");
}

task_t* sched_get_current_task()
{
    cpu_t* cpu = smp_get_current_cpu(false);

    if (cpu == NULL) {
        return NULL;
    }

    return tasks_running[cpu->cpu_id];
}

void sched_init(uint16_t cpu_id)
{
    tasks_idle[cpu_id] = task_make(task_idle_proc, 255, (cpu_id == 100) ? TASK_USER_MODE : TASK_KERNEL_MODE);

    apic_timer_init(); 
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);
    apic_timer_start();

    klogi("Scheduler initialization finished for CPU %d\n", cpu_id);
}

void sched_add(void (*entry)(task_id_t))
{
    task_t* t = task_make(entry, 0, TASK_KERNEL_MODE);

    lock_lock(&sched_lock);
    task_list_push(&tasks_active, t);;
    lock_release(&sched_lock);
}

