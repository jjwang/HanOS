///-----------------------------------------------------------------------------
///
/// @file    klog.h
/// @brief   Definition of kernel log related functions
/// @details
///   A kernel-level log system was implemented. As the first step, it
///   mainly supports information display.
/// @author  JW
/// @date    Nov 20, 2021
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <device/term.h>

#define KLOG_BUFFER_SIZE       (UINT16_MAX + 1)

typedef struct {
    uint8_t buff[KLOG_BUFFER_SIZE];
    int start, end;
    term_info_t* term;
} klog_info_t;

void klog_init(klog_info_t* k, term_info_t* t);
void klog_printf(klog_info_t* k, const char*, ...);
void klog_refresh(klog_info_t* k);

