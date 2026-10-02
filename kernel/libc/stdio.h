/**-----------------------------------------------------------------------------

 @file    stdio.h
 @brief   Standard I/O definitions and declarations for HanOS standard library
 @details
 @verbatim

  This file provides the declarations for standard input/output functions and
  definitions for file system structures and constants used in HanOS. It includes
  definitions for standard file descriptors (STDIN, STDOUT, STDERR), the end-of-file
  marker (EOF), and file system structures such as timespec_t, dirent_t, and stat_t.
  Additionally, it defines various file types and modes.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once
#include <stdint.h>

#include <numeric.h>

#define STDIN               0
#define STDOUT              1
#define STDERR              2

#define EOF                 -1

/* ----- Definition of file system, same with vfs.h ----- */
#ifndef KERNEL_BUILD
#include <hanos.h>
#else
#include <fs/vfs.h>
typedef vfs_stat_t stat_t;
#endif /* NO KERNEL_BUILD */
/* ----- Definition of file system finished ----- */

int32_t fprintf(int32_t fd, const char *fmt, ...);
int32_t printf(const char *fmt, ...);

