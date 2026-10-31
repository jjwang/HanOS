# 2026-10-31: Resolve the remaining code TODOs

## Changes

- faccessat: validate flags; AT_EACCESS is the default check, others return EINVAL.
- sched: check a canary at the low end of every kernel stack on each switch; the fork, clone and create paths write it.
- madt: parse and log the NMI source record.
- mmap: reject MAP_FIXED below the first page, keeping the zero page unmapped.
- sched/kshell: drop the stale idle and exception-probe comments.
- kmalloc: document that zeroing is deliberate.

## Verification

- GCC 10.2, TCG: boot markers pass on `-smp 1/4`; musltest, faulttest, cloexectest and exitgrouptest pass; MADT logs `NMI source 255 lint 1`.
