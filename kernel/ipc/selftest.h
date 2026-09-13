/**-----------------------------------------------------------------------------

 @file    selftest.h
 @brief   Kernel-side self-test for the Phase 0 IPC/handle layer

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/task.h>

_Noreturn void mk_selftest_task(task_id_t tid);
