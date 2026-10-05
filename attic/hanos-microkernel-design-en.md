# HanOS: Architecture of a Hybrid Microkernel

*This article describes the current implementation. Revise it when the implementation changes.*

## Abstract

HanOS is an operating system for x86-64 written in C. It follows the Linux x86-64 syscall ABI while moving the filesystem, block device, terminal, pipe, file-descriptor and network subsystems into ring-3 servers. User space links musl. The kernel keeps scheduling, virtual memory, IPC, interrupt routing and the syscall entry points. Servers communicate over a capability-based IPC layer built on endpoints and handle transfer. Threads share an address space and block on a futex. This document describes the kernel primitives, the IPC mechanism, the service router, threads, and the implementation of each server.

## 1. Design

A monolithic kernel runs every subsystem in ring 0. A bug in the filesystem corrupts scheduler state or the page tables. A pure microkernel moves every driver and service to ring 3 but forces a large redesign of the ABI and of device access. HanOS takes the middle path.

- Follow the Linux x86-64 syscall ABI: POSIX calls keep their Linux numbers, HanOS-only calls use `0x400` and above, an error returns a negative errno in `RAX`, and `O_*` / `MAP_*` use the Linux values. musl runs on a thin backend.
- Move a subsystem to a server when its interface is bounded and its state can live in ring 3.
- Keep scheduling, address spaces, IPC, interrupts and the bootstrap loader in the kernel.

The kernel bootstraps the first servers from the boot initrd image. It builds no filesystem in ring 0. Once the VFS server registers, every file path goes through a server.

The result is a kernel that owns address spaces, process creation, IPC and interrupts, plus a set of cooperative servers that own file descriptors, filesystem state, pipes, the console, input devices and the network.

## 2. System overview

The system has three layers.

- The kernel runs in ring 0. It provides scheduling, virtual memory, the kernel object and handle model, IPC endpoints, interrupt objects, the service router and the syscall table.
- The servers run in ring 3 as ordinary processes. Each server owns one resource domain: the framebuffer, the PS/2 and serial input, the terminal, the ATA disk, a FAT32 volume, the initrd namespace, pipes, the per-process file descriptor table, or the network.
- The programs run in ring 3 and link musl; musl issues the syscalls.

Services are addressed by a service id. The kernel's router maps a service id to the endpoint of the owning server.

| Service | Id | Owner |
|---------|----|-------|
| SVC_MM | 0 | in-kernel (memory) |
| SVC_FS | 1 | `/bin/vfs` |
| SVC_PROC | 2 | `/bin/process` |
| SVC_NET | 3 | `/bin/net` |
| SVC_MISC | 4 | reserved |
| SVC_PIPE | 5 | `/bin/pipe` |
| SVC_TTY | 6 | `/bin/tty` |
| SVC_FAT | 7 | `/bin/fat32` |

## 3. Kernel primitives

### 3.1 Privilege model and boot

Limine loads the kernel ELF, the initrd and a configuration into memory and jumps to `kmain`. The kernel brings up the serial port, the ACPI tables, the local APIC, the HPET, the SMP cores, the page tables and the memory allocators. It records the initrd image and reads ACPI tables on demand. It creates no filesystem in ring 0. It then creates `kshell`, a kernel process that starts the servers and finally execs the shell program.

A server is created with `sched_execve` and receives a `bootinfo_t` block. The block holds handles to the endpoints the server needs, its granted interrupt line, I/O-port ranges, and the framebuffer, initrd or NIC mapping. A server reads it with the `bootinfo` syscall before it does anything else.

### 3.2 Kernel objects and handles

An endpoint, a memory object and an interrupt object are all `kernel_object_t`. The base header holds a type, an atomic reference count, a pointer to the implementation and a destroy function. Reference counting frees an object when the last reference drops.

User space reaches objects only through handles in a per-process handle table. A handle packs a 16-bit slot index and a 16-bit generation. The generation increments on every allocation, so a recycled slot never aliases a stale handle. Each slot also stores access rights.

- `handle_alloc` finds a free slot, bumps the generation and takes a reference.
- `handle_get` checks the index, the generation and the requested rights.
- `handle_close` clears the slot and drops the reference.
- `handle_dup` copies a handle with the same rights.

Handles move between processes when a message carries them in the `xfer[]` array. The kernel takes the object from the sender's table and moves it into the receiver with a restricted right set. Section 4 describes the transfer.

### 3.3 Endpoints and message passing

An endpoint is a kernel object that holds a FIFO of up to 64 messages. A message has a tag, six inline words, and up to two transferred handles. The inline words carry small payloads; bulk data travels in a memory object moved in `xfer[]`.

- `ipc_send` enqueues a message and wakes a receiver.
- `ipc_recv` blocks until a message is available.
- `ipc_recv_timeout` blocks with a deadline and returns on timeout.

Waking is explicit. `sched_wake_key` marks a sleeping process ready and, when the process lives on another core, sends a reschedule IPI to that core. Without the IPI a woken process would wait for the target core's timer tick. Section 4 gives the full mechanism.

### 3.4 The service router

The router holds an endpoint pointer and an owner pid per service id. A server registers its endpoint after it starts. A syscall that needs a service calls `router_forward`: it creates a reply endpoint, moves it into the request, sends the request, and blocks on the reply. If a service id has no endpoint, `router_lookup` returns NULL and the caller reports the unreachable service. Section 4.8 describes the router.

### 3.5 Memory objects and user-copy safety

A memory object (`memobj_t`) is a refcounted set of physical pages. The kernel creates one for a bulk transfer, moves its handle in `xfer[1]`, and the server maps it with `mem_map`, copies into it, and unmaps it. The kernel never passes a raw user pointer to a server.

Inside the kernel, copies between the kernel and a process address space use the uaccess helpers. Each faulting load or store has an entry in an exception table (`__ex_table`). The page-fault handler looks up the faulting instruction and resumes at its fixup label, so a bad user pointer returns `-EFAULT` instead of killing the kernel. A fault that did not come from a uaccess fixup and reached user mode terminates the process group with the matching signal (SIGSEGV, SIGILL, SIGFPE, SIGBUS); the core keeps running. A kernel-mode fault without a fixup panics.

### 3.6 Scheduling

Each core owns a run queue and a current process. A 1 ms APIC timer drives the time slice; each core installs the context-switch entry on its own timer vector. A process that blocks is put on the queue with a wakeup deadline. The switch code promotes expired sleepers to ready before it picks the next process, so a perpetually runnable process cannot starve timed sleepers. Wakeups use the reschedule IPI described in Section 4.4.

### 3.7 Memory layout

- The kernel direct map covers RAM only. `vmm_init` maps the usable entries, the bootloader and ACPI reclaimable regions, the kernel image and the framebuffer. It does not map the MMIO holes.
- ACPI tables live in reserved memory. `acpi_init` maps each table before it reads it.
- User programs link at `0x0000400000000000`, so the kernel and user mappings never share the HHDM range.
- The process server owns the per-pid file descriptor table; the kernel resolves a fd to a server descriptor on demand.

### 3.8 Threads and futex

A thread is a `process_t` that shares its address space with the group. `clone` with `CLONE_VM` creates one. The kernel refcounts the address space (`addrspace_t.refs`) and keeps the mapping list in the address space, so the last thread frees it. A thread carries the group id `tgid`; `pid == tgid` for the leader.

`process_clone` allocates a kernel stack and builds the child's user-mode return frame on it. The syscall entry runs on the user stack, so a thread must not reuse the parent's. The frame sets `rax = 0`, `rip` to the instruction after `syscall`, and `rsp` to the stack `clone` passed. `CLONE_SETTLS` sets the child `fs_base`. `CLONE_PARENT_SETTID` and `CLONE_CHILD_SETTID` write the tid. `CLONE_CHILD_CLEARTID` records the address to clear and wake on thread exit.

`exit` ends one thread. `exit_group` marks every other thread of the group dead, lets the idle reaper free them, then exits the caller.

A futex blocks on a user word. `k_futex_wait` re-reads the word after it arms the wait key, so the loop still observes a wake delivered in the window. `k_futex_wake` wakes up to `nr` waiters. Both build on the IPC wait-key mechanism, so a wake sends a reschedule IPI to the waiter's core.

The kernel keys file-descriptor calls by `tgid`, so every thread of a group shares the leader's fd table.

### 3.9 Signals

The syscall entry saves the user register frame on the user stack. After the handler runs, the kernel checks the signal state of the current process. A pending, unblocked signal with a handler makes the kernel push a signal frame on the user stack, point the return RIP at the handler, pass the signal number in the first argument register, and return through `sysret`. The restorer the action carries runs `rt_sigreturn`, which reads the frame back, restores the interrupted register set, and resumes the syscall. The syscall result survives in the frame.

`kill`, `tkill` and `tgkill` queue a signal and wake the target. With no handler the kernel applies the default action: ignore, or terminate the process. The handler blocks its own signal unless `SA_NODEFER`, plus the action mask. `SIGKILL` bypasses the mask and the handler.

The frame belongs to `kernel/proc/signal.c`; `signal_deliver` takes the interrupted RIP and RFLAGS explicitly, so the syscall path passes them from `rcx`/`r11` through `syscall_post` in `kernel/proc/syscall.c` and the interrupt path passes `rip`/`rflags`. The APIC timer preemption entry `enter_context_switch` calls the same delivery before it selects the next process, so a signal raised while a process runs in user mode lands on the next timer tick, not only on a syscall return. A default action on that path marks the process dead for the scheduler to drop; the idle reaper records its exit, because the process-server round trip cannot run with interrupts disabled.

The kernel stores a Linux wait status. A normal exit stores the low byte shifted up; a signal death stores the signal in the low bits with `0x80` for a core dump. The parent's `wait` returns that status, so `WIFEXITED`/`WEXITSTATUS` and `WIFSIGNALED`/`WTERMSIG` decode it.

### 3.10 Interrupt controllers

The kernel enables the local APIC and programs the I/O APIC, so a device line reaches the CPU on real hardware. It parses the MADT interrupt source overrides and maps the first I/O APIC. Each enabled ISA line gets a redirection entry that carries the PIC-compatible vector (`0x20 + IRQ`) to the boot core. `irq_clear_mask` routes through the I/O APIC when one is present and keeps the 8259 line masked; the 8259 path remains for a machine without an I/O APIC. The cascade line (IRQ2) is not routed, because an override may map its GSI to another line. The interrupt return sends a local-APIC EOI when the I/O APIC delivered the interrupt, and a PIC EOI otherwise.

## 4. The IPC mechanism

Every request between a client and a server travels as a message on an endpoint. A pointer to user memory never crosses the boundary. This section describes the message, the endpoint, handle transfer, the reply pattern, blocking, deferred replies, bulk data, the router and interrupt delivery.

### 4.1 Endpoint

An endpoint is a kernel object that holds a spinlock and a FIFO of 64 messages.

```c
#define IPC_WORDS 6
#define IPC_QUEUE_LEN 64

struct endpoint {
    kernel_object_t obj;
    spinlock_t lock;
    ipc_queue_entry_t msgs[IPC_QUEUE_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};
```

The queue is a ring buffer. `ipc_send` appends at `tail`; `ipc_try_recv` pops at `head`. The spinlock guards the head, the tail and the count across cores. A full queue rejects the send.

### 4.2 Message

```c
#define IPC_WORDS 6

struct ipc_msg {
    uint64_t tag;
    uint64_t words[IPC_WORDS];
    handle_t xfer[2];
    uint8_t  xfer_count;
};
```

- The tag names the request. Each service defines its own tag space (`VFS_*`, `PIPE_*`, `PROC_*`, `NET_*`, `TTY_*`, `FAT_*`).
- Six inline words carry small arguments and small replies. A reply puts a status or a negative errno in `words[0]`.
- `xfer[]` moves up to two handles with the message. A path or a bulk buffer travels as a memory object; a reply endpoint travels as an endpoint object.
- `xfer_count` names how many entries of `xfer[]` the sender set. The receiver reads them after the copy.

### 4.3 Handle transfer

A handle is a per-process reference to a kernel object. The table packs a 16-bit slot and a 16-bit generation; the generation changes on reuse, so a stale handle never resolves.

```c
handle_t handle_alloc(handle_table_t *ht, kernel_object_t *o, uint32_t rights);
kernel_object_t *handle_get(handle_table_t *ht, handle_t h, uint32_t rights);
int handle_close(handle_table_t *ht, handle_t h);
```

Rights are `READ`, `WRITE`, `SEND`, `RECV`, `MAP` and `TRANSFER`. A sender hands over an object only when its handle carries `TRANSFER`. The kernel moves the object out of the sender's table into the queued message, then into the receiver's table with a restricted right set:

- memory object → `READ | WRITE | MAP`
- endpoint → `SEND | RECV`
- interrupt object → `READ`

The transfer is a move: the sender's handle closes when the message is queued. A received handle has no `TRANSFER` right, so a server cannot relay it onward.

### 4.4 Send, receive and wake

```c
int ipc_send(endpoint_t *ep, const ipc_msg_t *msg);
int ipc_recv(endpoint_t *ep, ipc_msg_t *msg);
int ipc_recv_timeout(endpoint_t *ep, ipc_msg_t *msg, time_t ms);
int ipc_recv_nb(endpoint_t *ep, ipc_msg_t *msg);
```

- `ipc_send` enqueues and calls `sched_wake_key(ep)`.
- `ipc_recv` blocks through `sched_wait_key_begin` and `sched_wait_key_commit`. The wait key is the endpoint pointer.
- `sched_wake_key` marks the sleeping process ready. When that process runs on another core, the waker sends a reschedule IPI, so the receiver runs on its own core without waiting for a timer tick.

The wake is not lost. `sched_wait_key_begin` arms the process before it checks the queue; `sched_wait_key_commit` sleeps only when the queue was empty. A wake delivered in the window marks the process ready, and the receive loop observes it.

### 4.5 The reply pattern

A caller that needs an answer uses `router_forward`. The kernel creates an ephemeral reply endpoint and moves it into the request.

1. Create a reply endpoint.
2. Move the reply endpoint into `xfer[0]`; move the request's own handles after it in `xfer[1..]`.
3. Send the request to the service endpoint.
4. Block on the reply endpoint with a deadline.
5. Drop the reply endpoint and return the reply message.

The server reads the reply endpoint from `xfer[0]`, handles the request, and sends the reply on it. The server then closes the reply endpoint; the kernel drops its own reference on return. The reply path is one-way: a server never needs the caller's endpoint.

### 4.6 Deferred replies

A server that cannot answer at once keeps the reply endpoint and answers later. The reply endpoint is a handle, so holding it is enough. This is how the system blocks without polling.

- tty read with no key: the server stores the reply endpoint and the length, and answers on the next key.
- pipe read with no data, or write with no room: the server stores the reply endpoint and the memory object, and answers when the other end moves data or closes.
- datagram or stream receive with no data: the server stores the reply endpoint and the buffer, and answers when a segment arrives.
- `accept` with no pending connection: the server stores the reply endpoint and answers when a connection completes.

Each server holds at most a few deferred requests, so the memory cost is bounded.

### 4.7 Bulk data

A path or a file buffer never travels in the six inline words for large sizes. The caller creates a memory object (`mem_alloc`), maps it, fills it, and moves its handle in `xfer[1]`. The server maps it with `mem_map`, copies in or out, and unmaps it. The kernel never passes a user pointer across the boundary.

The VFS server uses one buffer for a path and a stat result: the path at offset 0, the result at `VFS_IO_DATA_OFF`. The pipe server keeps small transfers inline (up to 32 bytes) and uses a memory object above that. The network server moves packet bytes through a memory object.

### 4.8 The service router

The router is a directory from service id to endpoint.

```c
typedef enum {
    SVC_MM, SVC_FS, SVC_PROC, SVC_NET, SVC_MISC, SVC_PIPE, SVC_TTY, SVC_FAT,
    SVC_COUNT
} service_id_t;

void router_register(service_id_t id, endpoint_t *ep, pid_t owner);
endpoint_t *router_lookup(service_id_t id);
bool router_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep);
bool router_forward_timeout(service_id_t id, const ipc_msg_t *req,
                            ipc_msg_t *rep, time_t timeout_ms);
```

A server registers its endpoint after it starts. A syscall looks up the service and forwards. `router_lookup` returns NULL when no service is registered; the caller then reports the unreachable service. Only the initrd reader and the in-kernel device drivers remain in ring 0.

### 4.9 Interrupts as messages

An interrupt object (`OBJ_IRQ`) is created for a line. `irq_bind` attaches an endpoint. When the line fires, `irq_deliver` sends an `IRQ_NOTIFY_TAG` message to that endpoint; the ISR path skips the in-kernel handler for a bound line. The driver acknowledges with `irq_ack`. A hardware event reaches a server through the same message path as a syscall.

### 4.10 A file read end to end

1. musl issues the Linux `read` syscall (0) with `(fd, buf, len)`.
2. The kernel resolves `fd` through the process server into `(svc, server_fd)`.
3. The kernel creates a memory object for `buf` and builds `VFS_READ`.
4. `router_forward` moves a reply endpoint and the memory object, and sends to the FS endpoint.
5. The VFS server maps the object, copies the bytes, replies with the count, and closes the reply endpoint.
6. The kernel copies the object back into `buf` and returns the count.

## 5. System servers

Each server is a process that receives on `service_ep`, handles a request, and replies on the endpoint the kernel moved in `xfer[0]`. Bulk payloads use a memory object in `xfer[1]`.

### 5.1 console

The console server owns the framebuffer. The kernel grants it a mapping of the scan-out with write-combining attributes and an endpoint for console bytes. The server keeps a back buffer and renders text with the shared gohufont glyphs. It tracks dirty rows and copies only those to the framebuffer.

The kernel and the tty server send `CONSOLE_WRITE_TAG` messages. The server decodes a minimal SGR subset (colours) and a newline, carriage return, backspace and tab. When the queue is idle it blinks a block cursor. The server starts from a cleared screen, so it does not inherit the boot splash.

### 5.2 input

The input server owns the PS/2 controller, its IRQ lines and COM1. The kernel grants it IRQ1 (keyboard), IRQ12 (mouse) and IRQ4 (serial), the PS/2 ports `0x60` and `0x64`, and the COM1 range `0x3F8` to `0x3FF`. All port access goes through the range-checked `ioport_access` syscall.

On an interrupt notification the server drains the PS/2 controller. The status byte says whether the byte came from the keyboard or the mouse. Keyboard scancodes are decoded through the shared keycode table; the mouse packets are three-byte deltas. The server also drains any COM1 bytes, so a serial console works without a PS/2 keyboard. Decoded keys and mouse deltas are sent to the kernel as `INPUT_KEY_TAG` and `INPUT_MOUSE_TAG` messages.

The kernel enumerates USB through a minimal xHCI driver in `kernel/device/usb/xhci.c`. At boot it resets the controller and each port, addresses the device, and reads the device and configuration descriptors. It scans every port and logs each device as vendor:product plus its interface class, so a USB network or wireless adapter appears in the boot log without a driver. A HID boot keyboard or pointer is registered with an interrupt endpoint and decoded into the same key path.

### 5.3 tty

The tty server owns `/dev/tty`. It buffers keys and echoes them to the console. The kernel relays each decoded key to the server as a `TTY_KEY` message.

Reads are event-driven and line-buffered. A `TTY_READ` with pending keys returns the bytes at once. A `TTY_READ` with no keys is deferred: the server keeps the reply endpoint and answers it when a line is complete or the requested length is buffered. The kernel blocks in `tty_server_read` on that reply instead of polling. A reader that asks for few bytes stays unbuffered.

`poll` on fd 0 asks the server for the pending key count through `TTY_POLL`. With no key it blocks on a kernel wait key that the tty relay wakes when a key arrives, so a poller sleeps instead of spinning. `select` and `pselect6` share the readiness helper. Section 5.7 and section 5.9 describe the pipe and socket readiness queries.

### 5.4 block

The block server owns the AHCI (SATA) controller. The kernel grants it the ABAR MMIO window and a physically contiguous DMA region, like the NIC. It answers `BLOCK_GET_INFO`, `BLOCK_READ` and `BLOCK_WRITE`. Reads and writes carry a memory object in `xfer[1]` holding the sectors and use 48-bit LBA addressing. The server builds a command list, a command table and a received-FIS area in the DMA region, issues `IDENTIFY DEVICE` and `READ`/`WRITE DMA EXT`, and polls the port. It copies through its own DMA buffer because only that memory has a known physical address, and never dereferences a client pointer. A bounded poll fails fast when the device is absent.

The server parses the GPT at start and records each used entry's LBA range. `BLOCK_GET_PART` returns a partition's start and sector count by index, so a filesystem server can mount a partition without parsing the table itself.

### 5.5 fat32

The FAT32 server is a read-only client of the block server. At start it reads the boot sector, validates the FAT32 signature and records the geometry. It walks directories with 8.3 names and follows a file's cluster chain.

Descriptors are server state. `FAT_OPEN` parses a path and returns a descriptor; `FAT_READ` advances its offset; `FAT_SEEK` repositions it; `FAT_FSTAT` reports size and directory flag; `FAT_READDIR` lists one entry. `FAT_STAT` reports a path without opening it. Paths travel at offset 0 of a buffer memory object and file data at `VFS_IO_DATA_OFF`.

### 5.6 vfs

The VFS server serves the initrd namespace. At start it parses the ustar archive the kernel mapped read-only and builds an index of files, directories and sizes. It also keeps a small set of files created at runtime in RAM.

The server owns path resolution and the FAT mount. A path request carries the process working directory and the path; the server joins and normalizes them. When the result is under `/fat`, the server writes the mount-relative path back and replies `VFS_REDIRECT_FAT`; the kernel then issues the request to the FAT server. The kernel builds no path.

The protocol covers `VFS_OPENAT`, `VFS_READ`, `VFS_WRITE`, `VFS_SEEK`, `VFS_CLOSE`, `VFS_READDIR`, `VFS_FSTAT`, `VFS_FSTATAT`, `VFS_FACCESSAT`, `VFS_UNLINK`, `VFS_MKDIRAT`, `VFS_SYMLINKAT`, `VFS_RENAMEAT`, `VFS_READLINK` and `VFS_FD_FORK`. A request that carries a path or a data buffer puts it in a memory object in `xfer[1]`; the server maps it, reads or fills it, and unmaps it. `VFS_FD_FORK` adds a reference so a forked child shares the open file description.

`VFS_MKDIRAT` creates a runtime directory. `VFS_SYMLINKAT` creates a runtime symlink and stores the target. `VFS_READLINK` returns that target. `VFS_RENAMEAT` renames a runtime entry and rewrites the prefixes of a renamed directory's children. The three calls reply `VFS_REDIRECT_FAT` for a path under the read-only FAT mount, which the kernel reports as `EROFS`.

### 5.7 pipe

The pipe server owns a small set of byte-stream pipes. `PIPE_CREATE` returns a read end and a write end. Reads and writes move data inline up to 32 bytes or through a memory object in `xfer[1]`. A read with no data whose write end is open, or a write with no room, is held: the server keeps the reply endpoint and the object, and answers when the other end moves data or closes. A read returns 0 once the write end closes. `PIPE_POLL` reports whether an end is readable (data or EOF) and writable. `VFS_FD_FORK` bumps the end reference count so a forked child shares the pipe.

### 5.8 process

The process server owns the per-process file descriptor table and the process tree. Each fd request names a pid and a fd.

- `PROC_FD_OPEN` registers a descriptor that names a service and a server fd; `PROC_FD_GET` resolves it; `PROC_FD_CLOSE` removes it and returns the server fd; `PROC_FD_DUP` copies it; `PROC_FD_FORK` clones a parent's table for a child; `PROC_FD_EXIT` closes the descriptors of an exiting process.
- `PROC_FD_FCNTL` reads and writes the close-on-exec flag. exec closes marked descriptors in the child.
- `PROC_EXEC` loads an ELF: the kernel packs the path, the working directory, the argv and the image into one memory object, and the server maps the segments, builds the stack, and starts the child.
- `PROC_EXIT` records an exit status; `PROC_WAIT` returns a dead child, blocks while a child is live, or reports `ECHILD`. A process counts as exited only after it exits and all its children are gone.

The kernel resolves a fd through the process server into a per-CPU transient descriptor. The kernel keys fd calls by `tgid`, so every thread of a group shares the leader's table. `dup3` of fd 0, 1 or 2 backs the new descriptor with the tty server, because the standard descriptors live in the kernel; reads and writes on the duplicate reach the tty. A socket registers a process fd as well, so `poll`, `select` and `epoll` reach it by the same resolution. `eventfd` and `epoll` objects live in the kernel and register a process fd with a kernel service id; `read`, `write` and `close` on them stay in ring 0.

A `poll` or `epoll_wait` that cannot return at once arms the process poll key and registers it with each polled pipe end, kernel eventfd, or socket. The `PIPE_POLL_WAIT` and `NET_POLL_WAIT` requests carry the key and the requested event bits; the server wakes the key only when the descriptor matches, and the kernel re-checks readiness after arming to close the race. The tty keeps its own key. `SYSCALL_POLL_WAKE` wakes a parked poller from a server.

### 5.9 net

The network server owns the socket layer and the NIC. It serves AF_INET datagram and stream sockets.

- Datagram: `sendto` to the loopback address is delivered in the server; a real address resolves ARP and emits a UDP/IP/Ethernet frame. A received datagram matches a bound socket.
- Stream: `connect` runs the SYN/SYN-ACK/ACK handshake; `send` emits a PSH segment; `recv` buffers incoming data; `close` sends FIN. `listen` and `accept` accept an incoming connection.
- Stream reliability: a per-connection send queue retransmits the oldest segment on timeout with backoff; out-of-order segments wait until the gap fills; the advertised window follows the free receive space; a FIN/ACK handshake closes the connection and keeps buffered data readable in CLOSE_WAIT.
- Receive validation: the server verifies the IPv4, TCP and UDP checksums and drops a bad frame.
- Socket options: `setsockopt`/`getsockopt` carry `SO_REUSEADDR`, `SO_RCVTIMEO` and `SOCK_NONBLOCK`; a timed or non-blocking read returns EAGAIN.
- Address configuration: a DHCP client takes the address, gateway and DNS server; a resolver sends an A-record query to that server and a HanOS resolve syscall returns the address.
- `NET_POLL` reports whether a socket has a buffered datagram, a completed connection, or a closed stream (readable) and whether a stream is established (writable). A socket registers a process fd, so `poll` and `epoll` reach it, and the server wakes a registered poll key when the state matches.
- The server drives the e1000e: the kernel grants the MMIO BAR and a physically contiguous DMA region; the server programs the rings and reads its MAC. The kernel binds the NIC's PCI interrupt line to the service endpoint and routes it through the I/O APIC (active low, edge), so the server drains the ring on the notification, with the poll as a fallback.
- An ARP cache backs address lookups. An RX dispatcher handles ARP, ICMP echo, UDP and TCP. The server pings the gateway at start.

### 5.10 ext2

The ext2 server is a read-only client of the block server. It mounts partition index 1 through `BLOCK_GET_PART`, reads the superblock at byte offset 1024, the block group descriptors and the inode table, and parses the direct and the single, double and triple indirect block maps. It serves `EXT2_OPEN`, `EXT2_READ`, `EXT2_READDIR`, `EXT2_STAT`, `EXT2_CLOSE` and `EXT2_SEEK`. Names keep their case; each inode carries a mode and an owner, and `STAT` reports the mode. The VFS mount table maps `/data` to this volume, so a path below `/data` is redirected to the ext2 server, the same way `/fat` reaches the FAT server. The partition holds applications and games, so they load from disk instead of the initrd.

## 6. Boot sequence

`kshell` starts the servers in dependency order.

1. console, so kernel output has a destination.
2. input and tty, so the terminal works.
3. block, then fat32 as its client.
4. vfs for the initrd namespace.
5. pipe.
6. process, which owns file descriptors and the process tree.
7. net, which owns the NIC.

It then execs `/bin/init`, which execs the shell. Each start registers the service endpoint with the router and probes the server with a ping.

The kernel loads the first servers from the initrd image with `vfs_load_file`. Once the VFS server registers, every file path goes through a server. No in-kernel filesystem remains; the kernel keeps only the initrd reader.

## 7. Limitations

- There is a single instance of each service; a crash of a server stops that domain.
- The FAT32 server is read-only and handles 8.3 names.
- The block server drives AHCI (SATA) with DMA. A legacy ATA PIO port is no longer used.
- SVC_MM and SVC_MISC are reserved but unused.
- TCP has no congestion control; the send queue holds four segments and the receive window follows the free buffer space.
- The NIC wakes the server on its PCI interrupt line; a bounded poll remains as a fallback.
- VFS runtime files live in RAM and do not persist.
- The xHCI driver enumerates every port but drives only a HID boot keyboard and pointer. It uses one controller and skips a controller with 64-byte contexts. A USB network or wireless adapter is logged by vendor:product and class, not driven.
- A signal handler entered on the timer path returns through `rt_sigreturn` and `sysret`, so the interrupted `rcx` is not restored. The syscall ABI already clobbers `rcx`; only user code that interrupts with a live value in `rcx` observes it.
- `mkdirat`, `symlinkat` and `renameat` create runtime entries only; the initrd and FAT mounts stay read-only.
- A set of syscalls still returns `ENOSYS`, for example `signalfd`, `getppid`, `chmod`, and `symlinkat` on the FAT mount.

## 8. Conclusion

HanOS keeps address spaces, processes, IPC and interrupts in the kernel and moves filesystem, block, terminal, pipe, file-descriptor and network state into ring-3 servers. A capability-based handle model, endpoint IPC with handle transfer, a service router, deferred replies, and memory objects for bulk data together give a working hybrid microkernel without breaking the POSIX syscall ABI.

## References

- Source: `kernel/`, `userspace/`, `musl/`.
- Kernel C library: `kernel/lib/`.
- Shared wire headers: `include/protocol.h`, `include/bootinfo.h`, `include/syscall_nr.h`.
- Userspace runtime: `userspace/runtime/`, `userspace/include/`.
- musl port: `musl/syscall_arch.h`, `musl/__set_thread_area.s`, `musl/linker.ld`, `musl/build.sh`.
- Kernel IPC and objects: `kernel/ipc/{ipc.c,object.c,irq.c}`.
- Router: `kernel/router/router.c`.
- Servers: `userspace/servers/`.
