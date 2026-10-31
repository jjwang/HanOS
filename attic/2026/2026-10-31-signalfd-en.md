# 2026-10-31: signalfd

## Changes

- signal: add `SVC_SIGNALFD` and a signalfd object table in the kernel.
- signal: add `signalfd4`. A new fd stores the mask; an existing fd updates it.
- signal: a read dequeues a pending signal that the fd mask and the process mask both select, and returns `signalfd_siginfo`.
- signal: `poll` reports POLLIN while a masked, blocked signal is pending.
- syscall: add `SYSCALL_SIGNALFD4` and the table entry.
- kmain: run `signalfd_selftest` after the servers start; it logs `MK: signalfd PASS`.
- ci: require `MK: signalfd PASS`.
- musltest: block SIGUSR1, raise it, and read the signalfd.

## Verification

- GCC 13.1 and cross GCC 10.2, TCG, `-smp 1`: `MK: selftest PASS`, `MK: misc PASS`, `MK: signalfd PASS`, shell starts as `hansh`.
