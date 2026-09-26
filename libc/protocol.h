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

/* VFS server */
#define VFS_PING            0x30        /* reply: words[0] = VFS_PONG */
#define VFS_PONG            0x504f4e47ULL       /* "PONG" */
#define VFS_FACCESSAT       0x39        /* words[0]=mode, words[1..]="path\0";
                                           reply words[0]=0 or -errno */

/* Longest path that fits inline in words[1..5]. */
#define VFS_INLINE_PATH     39

/* Every VFS request carries the reply endpoint handle in xfer[0] (moved by the
 * kernel's service_forward); the server replies on it and closes it. */
