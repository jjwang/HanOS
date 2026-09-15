/**-----------------------------------------------------------------------------

 @file    selftest.h
 @brief   Kernel-side self-test for the IPC/handle layer

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/process.h>

_Noreturn void mk_selftest_process(pid_t pid);
