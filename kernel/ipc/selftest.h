/**-----------------------------------------------------------------------------

 @file    selftest.h
 @brief   Kernel-side self-test for the IPC/handle layer
 @details
 @verbatim

  Declares the process that runs a boot-time round trip through endpoints
  and handle passing to catch regressions in the IPC core.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <proc/process.h>

_Noreturn void mk_selftest_process(pid_t pid);
