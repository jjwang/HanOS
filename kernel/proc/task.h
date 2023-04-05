/**-----------------------------------------------------------------------------

 @file    task.h
 @brief   Definition of task related functions
 @details
 @verbatim

  Create and return task data structure which contains registers and other task
  related information.

  Below is interrupt related information.

  When the CPU calls the interrupt handlers, it changes the value in the RSP
  register to the value specified in the IST, and if there is none, the stack
  stays the same. Onto the new stack, the CPU pushes these values in this order:

    SS:RSP (original RSP) -> RFLAGS -> CS -> RIP

  CS is padded to form a quadword. RIP stores code address.

  If the interrupt is called from a different ring, SS is set to 0, indicating a
  null selector. The CPU will modify the RFLAGS register, setting the TF, NT, and
  RF bits to 0. If the gate type is a trap gate, the CPU will clear the interrupt
  flag.

  The CPU will load the segment-selector value from the associated IDT descriptor
  into CS, and check to ensure that CS is a valid code segment selector.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lib/time.h>
#include <lib/vector.h>
#include <sys/smp.h>
#include <sys/mm.h>
#include <fs/vfs.h>

#define DEFAULT_KMODE_CODE      0b00101000 /* 0x28 */
#define DEFAULT_KMODE_DATA      0b00110000 /* 0x30 */

/*
 * User Mode Refer:
 *  http://jamesmolloy.co.uk/tutorial_html/10.-User%20Mode.html
 *
 * The x86 is strange in that there is no direct way to switch to user mode. The
 * only way one can reach user mode is to return from an exception that began in
 * user mode. The only method of getting there in the first place is to set up the
 * stack as if an exception in user mode had occurred, then executing an exception
 * return instruction (IRET).
 *
 *  - The instruction to continue execution at - the value of EIP.
 *  - The code segment selector to change to.
 *  - The value of the EFLAGS register to load.
 *  - The stack pointer to load.
 *  - The stack segment selector to change to.
 *
 * The EIP, EFLAGS and ESP register values should be easy to work out, but the
 * CS and SS values are slightly more difficult.
 *
 * When we set up our GDT we set up 5 selectors - the NULL selector, a code segment
 * selector for kernel mode, a data segment selector for kernel mode, a code
 * segment selector for user mode, and a data segment selector for user mode.
 *
 * They are all 8 bytes in size, so the selector indices are:
 *
 *  - 0x00: Null descriptor
 *  - 0x08: Kernel code segment
 *  - 0x10: Kernel data segment
 *  - 0x18: User code segment
 *  - 0x20: User data segment
 *
 * For user mode we want to use selectors 0x18 and 0x20. However, it's not quite
 * that straightforward. Because the selectors are all 8 bytes in size, the two
 * least significant bits of the selector will always be zero. Intel use these
 * two bits to represent the RPL - the Requested Privilege Level. These have
 * currently been zero because we were operating in ring 0, but now that we want
 * to move to ring three we must set them to '3'. If you wish to know more about
 * the RPL and segmentation in general, you should read the intel manuals. There
 * is far too much information for me to explain everything here.
 *
 * So, this means that our code segment selector will be (0x18 | 0x3 = 0x1b), and
 * our data segment selector will be (0x20 | 0x3 = 0x23).
 *
 */
#define DEFAULT_UMODE_DATA      0b00111011 /* 0x3b */
#define DEFAULT_UMODE_CODE      0b01000011 /* 0x43 */

/* ----- EFLAGS Register -----
 * 0        CF  Carry flag
 * 2        PF  Parity flag
 * 4        AF  Auxiliary flag
 * 6        ZF  Zero flag
 * 7        SF  Sign flag
 * 8        TF  Trap flag
 * 9        IF  Interrupt enable flag
 * 10       DF  Direction flag
 * 11       OF  Overflow flag
 * 12-13    IOPL    I/O privilege level
 * 14       NT  Nested task flag
 * 16       RF  Resume flag
 * 17       VM  Virtual 8086 mode flag
 * 18       AC  Alignment check
 * 19       VIF Virtual interrupt flag
 * 20       VIP Virtual interrupt pending
 * 21       ID  Able to use CPUID instruction
 *
 * You may notice that we disabled interrupts before starting the mode switch. A
 * problem now occurs - how do we re-enable interrupts? You'll find that
 * executing sti in user mode will cause a general protection fault, however if
 * we enable interrupts before we do our IRET, we may be interrupted at a bad
 * time.
 *
 * A solution presents itself if you know how the sti and cli instructions work
 * - they just set the 'IF' flag in EFLAGS. Wikipedia tells us that the IF flag
 * has a mask of 0x200, so what you could do, is insert these lines just after
 * the 'pushf' in the asm above:
 *
 *  pop %eax  ; Get EFLAGS back into EAX. The only way to read EFLAGS is to pushf
 *            ; then pop.
 *  or %eax, $0x200 ; Set the IF flag.
 *  push %eax ; Push the new EFLAGS value back onto the stack. 
 *
 */
#define DEFAULT_RFLAGS          0b0000001000000010 /* 0x0202 */

#define TID_MAX                 UINT64_MAX
#define TID_NONE                0

typedef uint64_t task_id_t;
typedef uint8_t task_priority_t;

typedef struct [[gnu::packed]] {
    uint64_t entry;
    uint64_t phdr;
    uint64_t phaddr;
    uint16_t phentsize;
    uint16_t phnum;
    uint64_t shdr;
    uint16_t shnum;
} auxval_t;

typedef enum {
    TASK_KERNEL_MODE,
    TASK_USER_MODE
} task_mode_t;

typedef enum {
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD
} task_status_t;

typedef struct [[gnu::packed]] {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} task_regs_t;

typedef enum {
    EVENT_UNDEFINED = 1,
    EVENT_KEY_PRESSED
} event_type_t;

typedef uint64_t event_para_t;

typedef struct {
    task_id_t       pub_tid;
    task_id_t       sub_tid;
    event_type_t    type;
    event_para_t    para;
    uint64_t        timestamp;
} event_t;

typedef struct task_t {
    void            *tstack_top;
    void            *tstack_limit;

    void            *kstack_top;
    void            *kstack_limit;

    void            *ustack_top;
    void            *ustack_limit;

    task_id_t       tid;
    task_id_t       ptid;
    task_priority_t priority;
    uint64_t        last_tick;
    uint64_t        wakeup_time;
    event_t         wakeup_event;
    task_status_t   status;
    task_mode_t     mode;

    auxval_t        aux;

    addrspace_t     *addrspace;
    vec_struct(mem_map_t) core_mmap_list;
    vec_struct(mem_map_t) mmap_list;

    char            cwd[VFS_MAX_PATH_LEN];
} task_t;

task_t* task_make(const char *name, void (*entry)(task_id_t), task_priority_t priority, task_mode_t mode);
task_t *task_fork(task_t *tp);
