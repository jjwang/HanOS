# 2026-10-23: Preserve GS base across syscall return

## Symptom

QEMU 4.2 with TCG faults on the first `syscall` from the console process. The fault is a page fault at `syscall_handler`, address 0x10, before the kernel stack switch. KVM boots the same image.

## Cause

The user return path leaves `GS.base` at zero under TCG. The handler used the per-CPU GS slots without swapping GS first. After SYSRET, `KERN_GS_BASE` can also be lost, so the next syscall sees GS base zero again.

## Fix

- Swap GS at syscall entry before touching the per-CPU slots.
- Before SYSRET, copy the live kernel `GS.base` back into `KERN_GS_BASE`, then swap GS for the user context.
- Return through SYSRET as before.

The same image now boots under QEMU 4.2 TCG and KVM. The headless TCG test runs `pwd` successfully.
