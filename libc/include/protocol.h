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

/* VFS server. A request that carries a path puts "cwd\0path\0" at offset 0 of a
 * memory object in xfer[1]; the reply endpoint is always xfer[0]. Replies put 0
 * or -errno in words[0] (and e.g. the server fd in words[1]). When the path is
 * under the FAT mount the server writes the mount-relative path back into the
 * object and replies VFS_REDIRECT_FAT; the caller then issues the FAT request. */
#define VFS_REDIRECT_FAT    100         /* path belongs to the FAT mount */
#define VFS_OPENAT          0x30        /* words[0]=flags; in: cwd+path; out: fd */
#define VFS_READ            0x31        /* words[0]=fd, words[1]=len;
                                           out: words[1]=bytes, xfer[1]=data */
#define VFS_WRITE           0x32        /* words[0]=fd, words[1]=len;
                                           in: xfer[1]=data */
#define VFS_CLOSE           0x33        /* words[0]=fd */
#define VFS_SEEK            0x34        /* words[0]=fd, words[1]=pos, words[2]=whence;
                                           out: words[1]=new position */
#define VFS_READDIR         0x35        /* words[0]=fd; out: words[0]=0 or -1 (end);
                                           xfer[1]=buffer, dirent at VFS_IO_DATA_OFF */
#define VFS_UNLINK          0x36        /* in: cwd+path */
#define VFS_FSTATAT         0x38        /* words[0]=flags; in: cwd+path in xfer[1];
                                           out: stat at VFS_IO_DATA_OFF */
#define VFS_FACCESSAT       0x39        /* words[0]=mode; in: cwd+path */
#define VFS_FSTAT           0x3A        /* words[0]=fd; out: stat at
                                           VFS_IO_DATA_OFF */
#define VFS_FD_FORK         0x3C        /* words[0]=fd; add a reference (fork) */

/* Buffer size and where a stat/dirent result is written inside the shared
 * buffer memory object. Inputs (such as a path) go at offset 0. */
#define VFS_IO_BUF_SIZE     8192
#define VFS_IO_DATA_OFF     4096

#define VFS_PING            0x3F        /* reply: words[0] = VFS_PONG */
#define VFS_PONG            0x504f4e47ULL       /* "PONG" */

/* Process server. It owns the per-process file-descriptor table; requests name
 * the process by pid (words[0]) and replies carry 0 or -errno in words[0]. */
#define PROC_PING           0x80        /* reply: words[0] = PROC_PONG */
#define PROC_PONG           0x504f4e47ULL       /* "PONG" */

/* fd table. PROC_FD_OPEN registers a new descriptor (svc/server_fd/size in
 * words[1..3], flags in words[4]) and returns the fd in words[1]. PROC_FD_GET
 * resolves a fd: out words[1]=kind (0 server, 1 kernel), words[2]=svc,
 * words[3]=server_fd, words[4]=size, words[5]=seek_pos. PROC_FD_CLOSE removes
 * it and returns the same fields so the caller can close the server side.
 * PROC_FD_DUP duplicates fd words[1] (or a free one when words[2] is -1). */
#define PROC_FD_OPEN        0x81
#define PROC_FD_CLOSE       0x82
#define PROC_FD_DUP         0x83
#define PROC_FD_GET         0x84
#define PROC_FD_SEEK        0x85        /* words[1]=pos, words[2]=whence */
#define PROC_FD_FORK        0x86        /* words[1]=child pid */
#define PROC_FD_EXIT        0x88
#define PROC_EXEC           0x89        /* words[0]=caller pid; xfer[1]=packed
                                           path/cwd/argv/envp and ELF bytes */
#define PROC_EXIT           0x8A        /* words[0]=pid, words[1]=status */
#define PROC_WAIT           0x8B        /* words[0]=parent, words[1]=target pid
                                           (-1 or 0 for any), words[2]=nohang;
                                           out words[0]=child pid, words[1]=status.
                                           Returns PROC_WAIT_BLOCK when a child
                                           exists but has not exited. */
#define PROC_WAIT_BLOCK     (-2)
#define PROC_FD_FCNTL       0x8C        /* words[1]=fd, words[2]=cmd, words[3]=arg;
                                           out words[1]=flags. cmd 1=F_GETFD,
                                           2=F_SETFD */

/* fcntl commands and descriptor flags handled by PROC_FD_FCNTL. */
#define F_GETFD             1
#define F_SETFD             2
#define FD_CLOEXEC          1

/* PROC_FD_GET/CLOSE kind. */
#define PROC_FD_SERVER      0
#define PROC_FD_KERNEL      1

/* Pipe server. Transfers up to PIPE_INLINE_MAX bytes travel inline in
 * words[2..]; larger ones in a memory object in xfer[1]. A read or write that
 * cannot make progress returns PIPE_EAGAIN in words[0]. */
#define PIPE_INLINE_MAX     32
#define PIPE_CREATE         0x43        /* out: words[1]=read fd, words[2]=write fd */
#define PIPE_READ           0x40        /* words[0]=fd, words[1]=len;
                                           out: words[1]=bytes (0 at EOF) */
#define PIPE_WRITE          0x41        /* words[0]=fd, words[1]=len */
#define PIPE_CLOSE          0x42        /* words[0]=fd */
#define PIPE_EAGAIN         (-11)

/* TTY server. There is a single tty, so reads and writes carry no fd. Data up
 * to TTY_INLINE_MAX bytes travels inline in words[2..], larger data in a
 * memory object in xfer[1]; a read with no pending key returns TTY_EAGAIN. */
#define TTY_INLINE_MAX      32
#define TTY_READ            0x60        /* words[0]=len; out: words[1]=n */
#define TTY_WRITE           0x61        /* words[0]=len; out: words[1]=n */
#define TTY_KEY             0x62        /* kernel relay: words[0]=key byte */
#define TTY_EAGAIN          (-11)

/* FAT32 server (a read-only block-server client). File descriptors are server
 * state: FAT_OPEN returns a fd, FAT_READ advances its offset. The path goes at
 * offset 0 and file data at VFS_IO_DATA_OFF of a buffer memory object in
 * xfer[1]; FAT_STAT (path) and FAT_FSTAT (fd) report size/is_dir. */
#define FAT_READ            0x70        /* words[0]=fd, words[1]=len;
                                           out: words[1]=bytes, words[2]=size */
#define FAT_STAT            0x71        /* in: path; out: words[1]=size, words[2]=is_dir */
#define FAT_READDIR         0x72        /* words[0]=fd, words[1]=index; out: -1 at
                                           end, else words[1]=size, words[2]=is_dir
                                           and the name at VFS_IO_DATA_OFF */
#define FAT_OPEN            0x73        /* in: path; out: words[1]=fd, words[2]=size */
#define FAT_CLOSE           0x74        /* words[0]=fd */
#define FAT_SEEK            0x75        /* words[0]=fd, words[1]=off, words[2]=whence;
                                           out: words[1]=pos */
#define FAT_FSTAT           0x76        /* words[0]=fd; out: words[1]=size, words[2]=is_dir */

/* Network server. Datagram sockets only; traffic to the loopback address is
 * delivered to the matching bound socket. A data buffer travels in xfer[1];
 * the reply endpoint is xfer[0]. A recvfrom with no data is held and answered
 * when a datagram arrives. */
#define AF_INET             2
#define SOCK_STREAM         1
#define SOCK_DGRAM          2
#define NET_IP_LOOPBACK     0x7f000001U /* 127.0.0.1 */
#define NET_PING            0x90
#define NET_SOCKET          0x91        /* words[0]=domain, words[1]=type;
                                           out words[1]=sock fd */
#define NET_BIND            0x92        /* words[0]=sock, words[1]=ip, words[2]=port */
#define NET_SENDTO          0x93        /* words[0]=sock, words[1]=ip, words[2]=port,
                                           words[3]=len; in xfer[1]=data;
                                           out words[1]=sent */
#define NET_RECVFROM        0x94        /* words[0]=sock, words[1]=len; in xfer[1]=buf;
                                           out words[1]=n, words[2]=src ip,
                                           words[3]=src port */
#define NET_CLOSE           0x95        /* words[0]=sock */
#define NET_CONNECT         0x96        /* words[0]=sock, words[1]=ip, words[2]=port;
                                           out words[0]=status */
#define NET_LISTEN          0x97        /* words[0]=sock */
#define NET_ACCEPT          0x98        /* words[0]=sock; out words[1]=new sock fd */

/* Every VFS request carries the reply endpoint handle in xfer[0] (moved by the
 * kernel's service_forward); the server replies on it and closes it. */
