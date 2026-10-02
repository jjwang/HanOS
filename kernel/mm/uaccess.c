/**-----------------------------------------------------------------------------

 @file    uaccess.c
 @brief   Implementation of safe user-space memory access helpers
 @details
 @verbatim

   The current HanOS address spaces map user pages into the process's own page
   tables, and the kernel runs with the process's CR3 while handling a syscall.
   A copy is therefore a fault-safe copy of the current process's memory, while
   a cross-process copy walks the page tables and reads through the kernel HHDM
   window.

   Each user access is annotated with a fixup in the __ex_table section (see
   linker.ld). If the access faults, the page-fault handler resumes at the fixup
   so the copy can report the number of bytes not copied instead of killing the
   kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <mm/mm.h>
#include <mm/uaccess.h>
#include <proc/process.h>
#include <proc/sched.h>

bool user_range_ok(process_t * t, const void *uptr, uint64_t len)
{
    if (len == 0)
        return true;

    if (t == NULL || t->addrspace == NULL)
        return false;

    uint64_t start = (uint64_t) uptr;
    uint64_t end = start + len;

    /* Reject wrap-around. */
    if (end < start)
        return false;

    for (uint64_t page = start & ~(uint64_t) (PAGE_SIZE - 1); page < end;
         page += PAGE_SIZE) {
        uint64_t pte = vmm_query(t->addrspace, page);
        if (!(pte & VMM_FLAG_PRESENT) || !(pte & VMM_FLAG_USER))
            return false;
    }

    return true;
}

/* --- Fault-safe primitive accesses --------------------------------------- */

/**
 * @brief Exception-table entry mapping a faulting instruction to its fixup
 */
typedef struct {
    uint64_t insn;
    uint64_t fixup;
} uaccess_ex_entry_t;

extern const uaccess_ex_entry_t __start___ex_table[];
extern const uaccess_ex_entry_t __stop___ex_table[];

uint64_t uaccess_find_fixup(uint64_t rip)
{
    for (const uaccess_ex_entry_t * e = __start___ex_table;
         e < __stop___ex_table; e++)
        if (e->insn == rip)
            return e->fixup;

    return 0;
}

/* Each macro returns 0 on success or 1 when the access faulted. The label 1 is
 * the faulting instruction and label 3 the fixup, registered in __ex_table. */

#define __uaccess_get8(dst, src) \
({ \
    uint64_t __v, __e = 0; \
    asm volatile( \
        "1:\tmovq %[m], %[v]\n" \
        "2:\n" \
        ".section .fixup,\"ax\"\n" \
        "3:\tmovq $1, %[e]\n" \
        "jmp 2b\n" \
        ".previous\n" \
        ".pushsection __ex_table,\"a\"\n" \
        ".balign 8\n" \
        ".quad 1b, 3b\n" \
        ".popsection\n" \
        : [v] "=r"(__v), [e] "+r"(__e) \
        : [m] "m"(*(const uint64_t *)(src)) \
        : "memory"); \
    if (!__e) \
        *(uint64_t *)(dst) = __v; \
    __e; \
})

#define __uaccess_get1(dst, src) \
({ \
    uint8_t __v; uint64_t __e = 0; \
    asm volatile( \
        "1:\tmovb %[m], %[v]\n" \
        "2:\n" \
        ".section .fixup,\"ax\"\n" \
        "3:\tmovq $1, %[e]\n" \
        "jmp 2b\n" \
        ".previous\n" \
        ".pushsection __ex_table,\"a\"\n" \
        ".balign 8\n" \
        ".quad 1b, 3b\n" \
        ".popsection\n" \
        : [v] "=q"(__v), [e] "+r"(__e) \
        : [m] "m"(*(const uint8_t *)(src)) \
        : "memory"); \
    if (!__e) \
        *(uint8_t *)(dst) = __v; \
    __e; \
})

#define __uaccess_put8(val, dst) \
({ \
    uint64_t __e = 0; uint64_t __v = (val); \
    asm volatile( \
        "1:\tmovq %[v], %[m]\n" \
        "2:\n" \
        ".section .fixup,\"ax\"\n" \
        "3:\tmovq $1, %[e]\n" \
        "jmp 2b\n" \
        ".previous\n" \
        ".pushsection __ex_table,\"a\"\n" \
        ".balign 8\n" \
        ".quad 1b, 3b\n" \
        ".popsection\n" \
        : [m] "=m"(*(uint64_t *)(dst)), [e] "+r"(__e) \
        : [v] "r"(__v) \
        : "memory"); \
    __e; \
})

#define __uaccess_put1(val, dst) \
({ \
    uint64_t __e = 0; uint8_t __v = (val); \
    asm volatile( \
        "1:\tmovb %[v], %[m]\n" \
        "2:\n" \
        ".section .fixup,\"ax\"\n" \
        "3:\tmovq $1, %[e]\n" \
        "jmp 2b\n" \
        ".previous\n" \
        ".pushsection __ex_table,\"a\"\n" \
        ".balign 8\n" \
        ".quad 1b, 3b\n" \
        ".popsection\n" \
        : [m] "=m"(*(uint8_t *)(dst)), [e] "+r"(__e) \
        : [v] "q"(__v) \
        : "memory"); \
    __e; \
})

uint64_t copy_from_user(void *kdst, const void *usrc, uint64_t len)
{
    uint8_t *d = kdst;
    const uint8_t *s = usrc;
    uint64_t done = 0;

    while (done + 8 <= len) {
        if (__uaccess_get8(d + done, s + done))
            return len - done;
        done += 8;
    }
    while (done < len) {
        if (__uaccess_get1(d + done, s + done))
            return len - done;
        done++;
    }

    return 0;
}

uint64_t copy_to_user(void *udst, const void *ksrc, uint64_t len)
{
    uint8_t *d = udst;
    const uint8_t *s = ksrc;
    uint64_t done = 0;

    while (done + 8 <= len) {
        uint64_t v;
        memcpy(&v, s + done, 8);
        if (__uaccess_put8(v, d + done))
            return len - done;
        done += 8;
    }
    while (done < len) {
        if (__uaccess_put1(s[done], d + done))
            return len - done;
        done++;
    }

    return 0;
}

uint64_t clear_user(void *udst, uint64_t len)
{
    uint8_t *d = udst;
    uint64_t done = 0;

    while (done + 8 <= len) {
        if (__uaccess_put8(0, d + done))
            return len - done;
        done += 8;
    }
    while (done < len) {
        if (__uaccess_put1(0, d + done))
            return len - done;
        done++;
    }

    return 0;
}

int64_t strncpy_from_user(char *kdst, const char *usrc, uint64_t max)
{
    if (max == 0)
        return -1;

    for (uint64_t i = 0; i < max; i++) {
        if (__uaccess_get1(kdst + i, usrc + i))
            return -1;
        if (kdst[i] == '\0')
            return (int64_t) i;
    }

    kdst[max - 1] = '\0';
    return -1;
}

uint64_t copy_from_process(process_t * t, void *kdst, const void *usrc,
                        uint64_t len)
{
    uint8_t *dst = (uint8_t *) kdst;
    uint64_t src = (uint64_t) usrc;
    uint64_t done = 0;

    if (t == NULL || t->addrspace == NULL)
        return len;

    while (done < len) {
        uint64_t paddr = vmm_get_paddr(t->addrspace, src);
        if (paddr == 0)
            return len - done;

        uint64_t off = src & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > len - done)
            chunk = len - done;

        memcpy(dst + done, (void *) PHYS_TO_VIRT(paddr + off), chunk);
        done += chunk;
        src += chunk;
    }

    return 0;
}

uint64_t copy_to_process(process_t * t, void *udst, const void *ksrc, uint64_t len)
{
    uint64_t dst = (uint64_t) udst;
    const uint8_t *src = (const uint8_t *) ksrc;
    uint64_t done = 0;

    if (t == NULL || t->addrspace == NULL)
        return len;

    while (done < len) {
        uint64_t paddr = vmm_get_paddr(t->addrspace, dst);
        if (paddr == 0)
            return len - done;

        uint64_t off = dst & (PAGE_SIZE - 1);
        uint64_t chunk = PAGE_SIZE - off;
        if (chunk > len - done)
            chunk = len - done;

        memcpy((void *) PHYS_TO_VIRT(paddr + off), src + done, chunk);
        done += chunk;
        dst += chunk;
    }

    return 0;
}
