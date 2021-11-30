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

void kpanic(const char* msg, ...);

