/**-----------------------------------------------------------------------------

 @file    uaccess.h
 @brief   Safe access to user-space memory
 @details
 @verbatim

   These helpers validate that a user pointer range belongs to a task and is
   mapped with user permissions before copying. They are the single entry point
   for every syscall that touches a user buffer, so the kernel can later run
   servers in separate address spaces without changing the call sites.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <proc/task.h>

/* True when [uptr, uptr+len) is fully mapped and user-accessible in task t. */
bool user_range_ok(task_t * t, const void *uptr, uint64_t len);

/* Copy to/from the *current* task. Return the number of bytes NOT copied
 * (0 on success) so callers can return -EFAULT. */
uint64_t copy_from_user(void *kdst, const void *usrc, uint64_t len);
uint64_t copy_to_user(void *udst, const void *ksrc, uint64_t len);
uint64_t clear_user(void *udst, uint64_t len);

/* Copy a NUL-terminated string from the current task. Returns the length on
 * success or -1 when the range faults or is not terminated within max. */
int64_t strncpy_from_user(char *kdst, const char *usrc, uint64_t max);

/* Cross-task copy used by IPC, where the kernel is not running on either
 * task's page tables. Translate through vmm_get_paddr() and the kernel HHDM. */
uint64_t copy_from_task(task_t * t, void *kdst, const void *usrc,
                        uint64_t len);
uint64_t copy_to_task(task_t * t, void *udst, const void *ksrc, uint64_t len);
