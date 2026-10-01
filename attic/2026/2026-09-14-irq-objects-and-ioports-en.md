# 2026-09-14: IRQ Objects, I/O-Port Grants and IPC Libc Wrappers

## Overview

Add kernel mechanisms for a userspace device driver. Expose the new IPC and capability syscalls to userspace. No IRQ line binds by default, so the in-kernel keyboard driver still runs.

## IRQ objects (`kernel/ipc/irq.{h,c}`)

- `irq_obj_t` wraps one interrupt line as a kernel object.
- `irq_create(irq)` registers the object in a per-line table.
- `irq_bind(io, ep)` binds it to an endpoint.
- `irq_ack(io)` counts acknowledgements.
- `irq_deliver(irq)` sends a short IPC message (`IRQ_NOTIFY_TAG`) to the bound endpoint.
- The object destructor clears its table slot. A freed object receives no further delivery.

## Interrupt routing (`kernel/sys/isr.c`)

For hardware lines IRQ0..IRQ15, `exc_handler_proc()` tries `irq_deliver(excno - IRQ0)` first. When a line is bound, the kernel sends the notification, issues EOI and skips the in-kernel handler. Otherwise the existing handler runs unchanged.

## I/O-port grants

- `task_t` gains `io_ports[4]` ranges and `io_port_count`.
- `IOPORT_ACCESS` (61) performs a range-checked `in`/`out` of width 1, 2 or 4 for the caller. A port outside every granted range returns `EPERM`.

## New syscalls

| # | Name | Purpose |
|---|------|---------|
| 57 | `IRQ_BIND` | bind an IRQ object handle to an endpoint handle |
| 58 | `IRQ_ACK` | acknowledge an IRQ object |
| 61 | `IOPORT_ACCESS` | range-checked port input/output |

## Userspace interface (`libc/sysfunc.{h,c}`)

- `sys_ipc_msg_t` mirrors the kernel `ipc_msg_t` layout.
- Wrappers added: `sys_ep_create`, `sys_ipc_send`/`recv`/`call`/`reply`, `sys_irq_bind`/`ack`, `sys_handle_close`, `sys_ioport_access`.

## Self-test

The kernel self-test creates an IRQ object on an unused line, binds it to an endpoint, calls `irq_deliver()` and checks that the notification arrives.

## Files Changed

| File | Change |
|------|--------|
| `kernel/ipc/irq.{h,c}` | add IRQ object and delivery |
| `kernel/sys/isr.c` | route bound IRQs to the endpoint |
| `kernel/proc/task.h` | grant I/O-port ranges |
| `kernel/proc/syscall.{h,c}` | add `IRQ_BIND`/`IRQ_ACK`/`IOPORT_ACCESS` |
| `kernel/ipc/selftest.c` | add IRQ delivery test |
| `libc/sysfunc.{h,c}` | add IPC/IRQ/port syscall wrappers |

## Testing

- `make` builds the kernel and userspace warning-free.
- `ENABLE_MICROKERNEL_SELFTEST` prints `MK: selftest PASS` on `-smp 2` and `-smp 4`.
- Boot and `ls` → `pwd` still work. No IRQ line binds, so the keyboard path is unaffected.
