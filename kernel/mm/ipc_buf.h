/**-----------------------------------------------------------------------------

 @file    ipc_buf.h
 @brief   Bulk-transfer helpers built on memory objects
 @details
 @verbatim

   Small IPC payloads travel inline in ipc_msg_t.words; larger ones (block and
   file I/O) are moved through a memory object: the kernel copies the payload
   into a fresh object and hands its handle to the server, or copies an object's
   contents back into a user buffer. The kernel is the only side that touches
   the object's physical pages, so servers never dereference client pointers.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <ipc/object.h>
#include <proc/process.h>

/* Copy a user buffer of process t into a fresh memory object; the handle is
 * allocated in the current process's table and returned in *out. */
int32_t ipc_buf_from_user(process_t * t, const void *uptr, uint64_t len,
                      handle_t * out);

/* Copy a memory object (handle owned by the current process) back into the user
 * buffer of process t, then close the handle. */
int32_t ipc_buf_to_user(process_t * t, handle_t h, void *uptr, uint64_t len);

/* Copy a kernel buffer into a fresh memory object (used by the ELF loader). */
int32_t ipc_buf_from_kernel(const void *kptr, uint64_t len, handle_t * out);
