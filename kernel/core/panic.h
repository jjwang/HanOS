///-----------------------------------------------------------------------------
///
/// @file    panic.h
/// @brief   Definition of panic related data structures
/// @details
///
///   A kernel panic is one of several boot issues. In basic terms, it is a
///   situation when the kernel can't load properly and therefore the system
///   fails to boot.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <lib/klog.h>

void dump_backtrace();

#define kpanic(s, ...)       { \
    asm volatile("cli"); \
    klog_rprintf(KLOG_LEVEL_ERROR, s, ##__VA_ARGS__); \
    dump_backtrace(); \
    while (true) \
        asm volatile("hlt"); \
} 

