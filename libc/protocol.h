/**-----------------------------------------------------------------------------

 @file    protocol.h
 @brief   IPC protocol tags shared by the kernel and the userspace servers
 @details
 @verbatim

   Message tags for the service protocols. Payloads travel in the message's
   inline words; bulk data (block/FS I/O) travels in a memory object whose
   handle is moved into the receiver via ipc_msg_t.xfer.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

/* Block server */
#define BLOCK_GET_INFO      0x22        /* reply: words[0]=sector_size,
                                           words[1]=sector_count */
#define BLOCK_READ          0x20        /* words[0]=lba, words[1]=count;
                                           xfer[0]=memobj to fill */
#define BLOCK_WRITE         0x21        /* words[0]=lba, words[1]=count;
                                           xfer[0]=memobj holding data */

/* Reply status carried in words[0] of a BLOCK_READ/WRITE reply. */
#define BLOCK_OK            0
#define BLOCK_ERR           (-1)

/* VFS server. A request that carries a path (or a data buffer) puts it in a
 * memory object in xfer[1]; the reply endpoint is always xfer[0]. Replies put
 * 0 or -errno in words[0] (and e.g. the server fd in words[1]). */
#define VFS_OPENAT          0x30        /* words[0]=flags; in: path; out: fd */
#define VFS_READ            0x31        /* words[0]=fd, words[1]=len;
                                           out: words[1]=bytes, xfer[1]=data */
#define VFS_WRITE           0x32        /* words[0]=fd, words[1]=len;
                                           in: xfer[1]=data */
#define VFS_CLOSE           0x33        /* words[0]=fd */
#define VFS_SEEK            0x34        /* words[0]=fd, words[1]=pos, words[2]=whence;
                                           out: words[1]=new position */
#define VFS_READDIR         0x35        /* words[0]=fd; out: words[0]=0 or -1 (end);
                                           xfer[1]=buffer, dirent at VFS_IO_DATA_OFF */
#define VFS_UNLINK          0x36        /* in: path */
#define VFS_FSTATAT         0x38        /* words[0]=flags; in: path in xfer[1];
                                           out: stat at VFS_IO_DATA_OFF */
#define VFS_FACCESSAT       0x39        /* words[0]=mode; in: path */
#define VFS_FD_FORK         0x3C        /* words[0]=fd; add a reference (fork) */

/* Buffer size and where a stat/dirent result is written inside the shared
 * buffer memory object. Inputs (such as a path) go at offset 0. */
#define VFS_IO_BUF_SIZE     8192
#define VFS_IO_DATA_OFF     4096

#define VFS_PING            0x3F        /* reply: words[0] = VFS_PONG */
#define VFS_PONG            0x504f4e47ULL       /* "PONG" */

/* Every VFS request carries the reply endpoint handle in xfer[0] (moved by the
 * kernel's service_forward); the server replies on it and closes it. */
