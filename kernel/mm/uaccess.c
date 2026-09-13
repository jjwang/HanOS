/**-----------------------------------------------------------------------------

 @file    uaccess.c
 @brief   Implementation of safe user-space memory access helpers
 @details
 @verbatim

   The current HanOS address spaces map user pages into the task's own page
   tables, and the kernel runs with the task's CR3 while handling a syscall.
   Therefore a "copy" is a range check followed by memcpy for the current task,
   while a cross-task copy walks the page tables and reads through the kernel
   HHDM window.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <mm/mm.h>
#include <mm/uaccess.h>
#include <proc/task.h>
#include <proc/sched.h>

bool user_range_ok(task_t * t, const void *uptr, uint64_t len)
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

uint64_t copy_from_user(void *kdst, const void *usrc, uint64_t len)
{
    task_t *t = sched_get_current_task();

    if (!user_range_ok(t, usrc, len))
        return len;

    memcpy(kdst, usrc, len);
    return 0;
}

uint64_t copy_to_user(void *udst, const void *ksrc, uint64_t len)
{
    task_t *t = sched_get_current_task();

    if (!user_range_ok(t, udst, len))
        return len;

    memcpy(udst, ksrc, len);
    return 0;
}

uint64_t clear_user(void *udst, uint64_t len)
{
    task_t *t = sched_get_current_task();

    if (!user_range_ok(t, udst, len))
        return len;

    memset(udst, 0, len);
    return 0;
}

int64_t strncpy_from_user(char *kdst, const char *usrc, uint64_t max)
{
    task_t *t = sched_get_current_task();

    if (max == 0)
        return -1;

    for (uint64_t i = 0; i < max; i++) {
        if (!user_range_ok(t, usrc + i, 1))
            return -1;
        kdst[i] = usrc[i];
        if (kdst[i] == '\0')
            return (int64_t) i;
    }

    kdst[max - 1] = '\0';
    return -1;
}

uint64_t copy_from_task(task_t * t, void *kdst, const void *usrc,
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

uint64_t copy_to_task(task_t * t, void *udst, const void *ksrc, uint64_t len)
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
