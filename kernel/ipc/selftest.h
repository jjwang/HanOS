/**-----------------------------------------------------------------------------

 @file    selftest.h
 @brief   Kernel-side self-test for the IPC/handle layer

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/task.h>

_Noreturn void mk_selftest_task(task_id_t tid);
