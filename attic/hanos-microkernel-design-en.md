# HanOS: Architecture of a Hybrid Microkernel

*This article describes the current implementation. Revise it when the implementation changes.*

## Abstract

HanOS is an operating system for x86-64 written in C. It keeps the classic POSIX syscall ABI while moving the filesystem, block device, terminal, pipe and file-descriptor subsystems into ring-3 servers. The kernel retains scheduling, virtual memory, IPC, interrupt routing and the syscall entry points. Servers communicate over a small capability-based IPC layer built on endpoints and handle transfer. This document describes the kernel primitives, the service router, and the implementation of each server.

## 1. Why a hybrid microkernel

A monolithic kernel runs every subsystem in ring 0. A bug in the filesystem can corrupt scheduler state or the page tables. A pure microkernel moves every driver and service to ring 3 but forces a large redesign of the ABI and of device access. HanOS takes the middle path.

- Keep the syscall numbers and names that userspace already uses.
- Move a subsystem to a server when it has a bounded interface and its state can live in ring 3.
- Keep an in-kernel implementation of the same interface. A syscall uses the server when the service is registered and the in-kernel path otherwise. This makes the migration incremental and keeps the system bootable at every step.

The result is a kernel that still owns address spaces, process creation, IPC and interrupts, plus a set of cooperative servers that own file descriptors, filesystem state, pipes, the console and the input devices.

## 2. System overview

The system has three layers.

- The kernel runs in ring 0. It provides scheduling, virtual memory, the kernel object and handle model, IPC endpoints, interrupt objects, the service router and the syscall table.
- The servers run in ring 3 as ordinary processes. Each server owns one resource domain: the framebuffer, the PS/2 and serial input, the terminal, the ATA disk, a FAT32 volume, the initrd VFS, pipes, or the per-process file descriptor table.
- The programs run in ring 3 and use the libc, which issues syscalls.

Services are addressed by a service id. The kernel's router maps a service id to the endpoint of the owning server.

| Service | Id | Owner |
|---------|----|-------|
| SVC_MM | 0 | in-kernel (memory) |
| SVC_FS | 1 | `/bin/vfs` |
| SVC_PROC | 2 | `/bin/process` |
| SVC_NET | 3 | reserved |
| SVC_MISC | 4 | reserved |
| SVC_PIPE | 5 | `/bin/pipe` |
| SVC_TTY | 6 | `/bin/tty` |
| SVC_FAT | 7 | `/bin/fat32` |

## 3. Kernel primitives

### 3.1 Privilege model and boot

Limine loads the kernel ELF, the initrd and a configuration into memory and jumps to `kmain`. The kernel brings up the serial port, the ACPI tables, the local APIC, the HPET, the SMP cores, the page tables and the memory allocators. It builds an in-kernel ramfs over the initrd, then creates `kshell`, a kernel process that starts the servers and finally execs the shell program.

A server is created with `sched_execve` and receives a `bootinfo_t` block. The block holds handles to the endpoints the server needs, its granted interrupt line, I/O-port ranges, and the framebuffer or initrd mapping. A server reads it with the `bootinfo` syscall before it does anything else.

### 3.2 Kernel objects and handles

An endpoint and a memory object are both `kernel_object_t`. The base header holds a type, an atomic reference count, a pointer to the implementation and a destroy function. Reference counting frees an object when the last reference drops.

User space reaches objects only through handles in a per-process handle table. A handle packs a 16-bit slot index and a 16-bit generation. The generation increments on every allocation, so a recycled slot can never alias a stale handle. Each slot also stores access rights.

- `handle_alloc` finds a free slot, bumps the generation and takes a reference.
- `handle_get` checks the index, the generation and the requested rights.
- `handle_close` clears the slot and drops the reference.
- `handle_dup` copies a handle with the same rights.

Handles move between processes when a message carries them in the `xfer[]` array. The router takes the object from the sender's table and moves it into the receiver with a restricted right set.

### 3.3 Endpoints and message passing

An endpoint is a kernel object that holds a FIFO of up to 64 messages. A message has a tag, six inline words, and up to two transferred handles. The inline words carry small payloads; bulk data travels in a memory object moved in `xfer[]`.

- `ipc_send` enqueues a message and wakes a receiver.
- `ipc_recv` blocks until a message is available.
- `ipc_recv_timeout` blocks with a deadline and returns on timeout.

Waking is explicit. `sched_wake_key` marks a sleeping process ready and, when the process lives on another core, sends a reschedule IPI to that core. Without the IPI a woken process would wait for the target core's timer tick.

### 3.4 The service router

The router holds an endpoint pointer and an owner pid per service id. A server registers its endpoint after it starts. A syscall that needs a service calls `router_forward`:

1. Create an ephemeral reply endpoint.
2. Move the reply endpoint in `xfer[0]` and the request's own handles after it.
3. Send the request to the service endpoint.
4. Block on the reply endpoint with a timeout.
5. Unref the reply endpoint and return the reply.

The callee replies on the reply endpoint and closes it. If a service id has no endpoint yet, `router_lookup` returns NULL and the caller runs the in-kernel implementation. This is the fallback that keeps the system bootable before the servers start and if a server is disabled.

### 3.5 Memory objects and user-copy safety

A memory object (`memobj_t`) is a refcounted set of physical pages. The kernel creates one for a bulk transfer, moves its handle in `xfer[1]`, and the server maps it with `mem_map`, copies into it, and unmaps it. The kernel never passes a raw user pointer to a server.

Inside the kernel, copies between the kernel and a process address space use the uaccess helpers. Each faulting load or store has an entry in an exception table (`__ex_table`). The page-fault handler looks up the faulting instruction and resumes at its fixup label, so a bad user pointer returns `-EFAULT` instead of killing the kernel.

### 3.6 Scheduling

Each core owns a run queue and a current process. A 1 ms APIC timer drives the time slice; each core installs the context-switch entry on its own timer vector. A process that blocks is put on the queue with a wakeup deadline. The switch code promotes expired sleepers to ready before it picks the next process, so a perpetually runnable process cannot starve timed sleepers. Wakeups use the reschedule IPI described in Section 3.3.

## 4. System servers

Each server is a process that receives on `service_ep`, handles a request, and replies on the endpoint the kernel moved in `xfer[0]`. Bulk payloads use a memory object in `xfer[1]`.

### 4.1 console

The console server owns the framebuffer. The kernel grants it a mapping of the scan-out with write-combining attributes and an endpoint for console bytes. The server keeps a back buffer and renders text with the shared gohufont glyphs. It tracks dirty rows and copies only those to the framebuffer.

The kernel and the tty server send `CONSOLE_WRITE_TAG` messages. The server decodes a minimal SGR subset (colours) and a newline, carriage return, backspace and tab. When the queue is idle it blinks a block cursor. The server starts from a cleared screen, so it does not inherit the boot splash.

### 4.2 input

The input server owns the PS/2 controller, its IRQ lines and COM1. The kernel grants it IRQ1 (keyboard), IRQ12 (mouse) and IRQ4 (serial), the PS/2 ports `0x60` and `0x64`, and the COM1 range `0x3F8` to `0x3FF`. All port access goes through the range-checked `ioport_access` syscall.

On an interrupt notification the server drains the PS/2 controller. The status byte says whether the byte came from the keyboard or the mouse. Keyboard scancodes are decoded through the shared keycode table; the mouse packets are three-byte deltas. The server also drains any COM1 bytes, so a serial console works without a PS/2 keyboard. Decoded keys and mouse deltas are sent to the kernel as `INPUT_KEY_TAG` and `INPUT_MOUSE_TAG` messages.

### 4.3 tty

The tty server owns `/dev/tty`. It buffers keys and echoes them to the console. The kernel relays each decoded key to the server as a `TTY_KEY` message.

Reads are event-driven. A `TTY_READ` with pending keys returns the bytes at once. A `TTY_READ` with no keys is deferred: the server keeps the reply endpoint and answers it when the next key arrives. The kernel blocks in `tty_server_read` on that reply instead of polling, so an idle shell makes no requests and is woken by the reply's reschedule IPI.

### 4.4 block

The block server owns the ATA PIO ports. It answers `BLOCK_GET_INFO`, `BLOCK_READ` and `BLOCK_WRITE`. Reads and writes carry a memory object in `xfer[1]` holding the sectors and use 28-bit LBA addressing. The server probes the primary master with `IDENTIFY`, reports the geometry, and never dereferences a client pointer. A bounded poll fails fast when the device is absent.

### 4.5 fat32

The FAT32 server is a read-only client of the block server. At start it reads the boot sector, validates the FAT32 signature and records the geometry. It walks directories with 8.3 names and follows a file's cluster chain.

Descriptors are server state. `FAT_OPEN` parses a path and returns a descriptor; `FAT_READ` advances its offset; `FAT_SEEK` repositions it; `FAT_FSTAT` reports size and directory flag; `FAT_READDIR` lists one entry. `FAT_STAT` reports a path without opening it. Paths travel at offset 0 of a buffer memory object and file data at `VFS_IO_DATA_OFF`.

### 4.6 vfs

The VFS server serves the initrd. At start it parses the ustar archive the kernel mapped read-only and builds an index of files, directories and sizes. It also keeps a small set of files created at runtime in RAM.

The protocol covers `VFS_OPENAT`, `VFS_READ`, `VFS_WRITE`, `VFS_SEEK`, `VFS_CLOSE`, `VFS_READDIR`, `VFS_FSTAT`, `VFS_FSTATAT`, `VFS_FACCESSAT`, `VFS_UNLINK` and `VFS_FD_FORK`. A request that carries a path or a data buffer puts it in a memory object in `xfer[1]`; the server maps it, reads or fills it, and unmaps it. `VFS_FD_FORK` adds a reference so a forked child shares the open file description.

The kernel's VFS layer holds only the per-CPU transient descriptor that names a server file, resolved from the process server by fd.

### 4.7 pipe

The pipe server owns a small set of byte-stream pipes. `PIPE_CREATE` returns a read end and a write end. Reads and writes move data inline up to 32 bytes or through a memory object in `xfer[1]`. A read with no data whose write end is open, or a write with no room, returns `PIPE_EAGAIN` and the kernel retries. A read returns 0 once the write end closes. `VFS_FD_FORK` bumps the end reference count so a forked child shares the pipe.

### 4.8 process

The process server owns the per-process file-descriptor table. Each request names a pid and a fd. `PROC_FD_OPEN` registers a descriptor that names a server and a server fd; `PROC_FD_GET` resolves it; `PROC_FD_CLOSE` removes it and returns the server fd so the caller can close the server side; `PROC_FD_DUP` copies a descriptor; `PROC_FD_SEEK` updates the offset; `PROC_FD_FORK` clones a parent's table for a child; `PROC_FD_EXIT` closes the descriptors of an exiting process.

The kernel no longer keeps a file table per process. A read, write, seek, close or stat on a server-backed fd resolves the fd through the process server into a per-CPU transient descriptor. Redirecting standard input and output uses the same table: `dup3` records an alias, and reads and writes on fds 0, 1 and 2 use the alias when one exists and the tty otherwise.

## 5. Boot sequence and fallback

`kshell` starts the servers in dependency order.

1. console, so kernel output has a destination.
2. input and tty, so the terminal works.
3. block, then fat32 as its client.
4. vfs for the initrd.
5. pipe.
6. process, which owns file descriptors.

It then execs `/bin/init`, which execs the shell. Each start registers the service endpoint with the router and probes the server with a ping.

Before a service registers, and if a server is disabled in `kconfig.h`, the router returns NULL and the syscall uses the in-kernel implementation. The kernel keeps in-kernel paths for the ramfs, the block device, pipes and the terminal, so the system boots and runs with no servers at all.

## 6. Limitations

- There is a single instance of each service; a crash of a server stops that domain. The in-kernel fallback covers the case where a server never starts, not the case where it dies.
- The FAT32 server is read-only and handles 8.3 names.
- The block server uses ATA PIO polling, not DMA.
- NET and MISC service ids are reserved but unused.
- Device servers poll their queues with a timeout; the tty read path is event-driven but the other servers are not.

## 7. Conclusion

HanOS keeps address spaces, processes, IPC and interrupts in the kernel and moves filesystem, block, terminal, pipe and file-descriptor state into ring-3 servers. A capability-based handle model, endpoint IPC with handle transfer, a service router with an in-kernel fallback, and memory objects for bulk data together give a working hybrid microkernel without breaking the POSIX syscall ABI.

## References

- Source: `kernel/`, `libc/`, `userspace/servers/`.
- Interface headers: `libc/include/protocol.h`, `libc/include/bootinfo.h`, `libc/include/sysfunc.h`.
- Kernel IPC and objects: `kernel/ipc/{ipc.c,object.c,irq.c}`.
- Router: `kernel/router/router.c`.
- Servers: `userspace/servers/`.
