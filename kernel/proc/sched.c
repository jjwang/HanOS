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

#define TIMESLICE_DEFAULT       MILLIS_TO_NANOS(1)

static lock_t sched_lock = {0};

static task_t* tasks_running[CPU_MAX] = {0};
static task_t* tasks_idle[CPU_MAX] = {0};
static uint64_t tasks_coordinate[CPU_MAX] = {0};

static task_list_t tasks_active = {0};

extern void enter_context_switch(void* v);
extern void exit_context_switch(task_t* next);

_Noreturn static void task_idle_proc(task_id_t tid)
{
    (void)tid;

    // This is for writing test of user space memory.
    uint64_t* temp_val = (uint64_t*)0x20001000;
    *temp_val = 0;

    while (true) {
        asm volatile ("nop;");
#if 0
        sleep(1000 * 10);
        cpu_t* cpu = smp_get_current_cpu();
        klogv("CPU %d idling (time coordinate %10d)\n",
              (cpu != NULL) ? cpu->cpu_id : -1,
              (cpu != NULL) ? tasks_coordinate[cpu->cpu_id] : 0);
#endif
    }
}

void do_context_switch(void* stack)
{
    lock_lock(&sched_lock);

    uint16_t cpu = smp_get_current_cpu()->cpu_id;
    uint64_t ticks = tasks_coordinate[cpu];

    task_t* curr = tasks_running[cpu];
    if (curr) {
        curr->kstack_top = stack;
        curr->last_tick = ticks;

        if (curr->status == TASK_RUNNING)
            curr->status = TASK_READY;
        task_list_push(&tasks_active, curr);;
    }

    task_t* next = task_list_pop(&tasks_active);;
    if (next == NULL) {
        next = tasks_idle[cpu];
    }

    next->status = TASK_RUNNING;
    tasks_running[cpu] = next;

    smp_get_current_cpu()->tss.rsp0 = (uint64_t)(next->kstack_limit + KSTACK_SIZE);
    tasks_coordinate[cpu]++;
    apic_send_eoi();

    lock_release(&sched_lock);

    exit_context_switch(next->kstack_top);
}

void sched_init(uint16_t cpu_id)
{
    tasks_idle[cpu_id] = task_make(task_idle_proc, 255, (cpu_id == 1) ? TASK_USER_MODE : TASK_KERNEL_MODE);

    apic_timer_init(); 
    apic_timer_set_period(TIMESLICE_DEFAULT);
    apic_timer_set_mode(APIC_TIMER_MODE_PERIODIC);
    apic_timer_set_handler(enter_context_switch);
    apic_timer_start();

    klogi("Scheduler initialization finished for CPU %d\n", cpu_id);
}

