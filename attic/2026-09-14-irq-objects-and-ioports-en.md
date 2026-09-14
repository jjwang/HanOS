# 2026-09-14: IRQ Objects, I/O-Port Grants and IPC Libc Wrappers

## Overview

Add the kernel mechanisms a userspace device driver needs, and expose the new
IPC/capability syscalls to userspace. No IRQ line is bound by default, so the
in-kernel keyboard driver still runs.

## IRQ objects (`kernel/ipc/irq.{h,c}`)

- `irq_obj_t` wraps one interrupt line as a kernel object.
- `irq_create(irq)` registers the object in a per-line table;
  `irq_bind(io, ep)` binds it to an endpoint; `irq_ack(io)` counts
  acknowledgements; `irq_deliver(irq)` sends a short IPC message
  (`IRQ_NOTIFY_TAG`) to the bound endpoint.
- The object destructor clears its table slot, so a freed object can never be
  delivered to again.

## Interrupt routing (`kernel/sys/isr.c`)

For hardware lines IRQ0..IRQ15, `exc_handler_proc()` now tries
`irq_deliver(excno - IRQ0)` first. When a line is bound, the notification is
sent, EOI is issued and the in-kernel handler is skipped; otherwise the
existing handler runs unchanged.

## I/O-port grants

- `task_t` gains `io_ports[4]` ranges and `io_port_count`.
- `IOPORT_ACCESS` (61) performs a range-checked `in`/`out` of width 1, 2 or 4 on
  behalf of the caller; a port outside every granted range returns `EPERM`.

## New syscalls

| # | Name | Purpose |
|---|------|---------|
| 57 | `IRQ_BIND` | bind an IRQ object handle to an endpoint handle |
| 58 | `IRQ_ACK` | acknowledge an IRQ object |
| 61 | `IOPORT_ACCESS` | range-checked port input/output |

## Userspace interface (`libc/sysfunc.{h,c}`)

- `sys_ipc_msg_t` mirrors the kernel `ipc_msg_t` layout.
- Wrappers added: `sys_ep_create`, `sys_ipc_send`/`recv`/`call`/`reply`,
  `sys_irq_bind`/`ack`, `sys_handle_close`, `sys_ioport_access`.

## Self-test

The kernel self-test now creates an IRQ object on an unused line, binds it to
an endpoint, calls `irq_deliver()` and checks that the notification arrives.

## Files Changed

| File | Change |
|------|--------|
| `kernel/ipc/irq.{h,c}` | new IRQ object and delivery |
| `kernel/sys/isr.c` | route bound IRQs to the endpoint |
| `kernel/proc/task.h` | granted I/O-port ranges |
| `kernel/proc/syscall.{h,c}` | `IRQ_BIND`/`IRQ_ACK`/`IOPORT_ACCESS` |
| `kernel/ipc/selftest.c` | IRQ delivery test |
| `libc/sysfunc.{h,c}` | IPC/IRQ/port syscall wrappers |

## Testing

- `make` (kernel and userspace) is warning-free.
- `ENABLE_MICROKERNEL_SELFTEST` prints `MK: selftest PASS` on `-smp 2` and
  `-smp 4`.
- Boot and `ls` → `pwd` still work; the keyboard path is unaffected because no
  IRQ line is bound.
