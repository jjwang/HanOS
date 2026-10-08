# 2026-10-09: Fork frame anchor rebase (user stack corruption)

## Symptom

A process dies at the first `ret` after it returns to user mode following a fork. The page fault names the low program address `0x400000001467`, the `ret` in `sys_write`, and a fault address equal to the stack pointer: a small value such as `0x202`, `0x206` or `0x246`. `0x202` is exactly the RFLAGS constant the context-switch stubs push.

The fault is rare on a light load. A loop of `ls /bin` stays clean. A mixed loop of `ls /`, `ls /bin`, `pwd`, `ls /bin | wc` and `cat /etc/version` hit it 8 times in 105 commands.

## Reproduction

- Boot `-smp 4`, type the mixed loop, and the fault arrives within a few dozen commands.
- Boot `-smp 1` with the same loop, and the fault still arrives. The cause is a logic fault, not a cross-CPU race.

## Investigation

Ruled out in turn:

- **In-place exec.** Disabling it (`if (0 && ...)`) so the old spawn path runs keeps the fault. The exec work is not the cause.
- **Signal frame rewrite.** `signal_deliver` returns on its fast path when no signal is pending, and the shell installs no handler.
- **Syscall return frame.** A `rsp < 1 MiB` bounds check in `syscall_post` never fired: no syscall returns with the bad stack pointer.
- **Context-switch boundary.** A check in `do_context_switch` inspected the frame of the process about to be restored and never saw a user frame with a bad `rsp`. Every switch in and out carries a good value.
- **process_fork rebasing.** The old code rebased the frame with `process_regs_t`; the rebased `context`, `rsp` and `rbp` always landed inside the child kernel stack, so a range check passed.

The value `0x202`, a RFLAGS constant, pointed at a frame read 8 bytes off. `push_all` pushes `r15` first and `rax` last, so the saved frame is `rax`-first. `process_regs_t` declares `r15` first. The two orders disagree.

## Root cause

A forked child does not enter the kernel through `syscall_handler`. It resumes at the `.exit` label inside `fork_context_switch`, restored by `exit_context_switch`. The syscall return path then runs `mov rsp, [r15 - 16]`, where `r15` is the frame anchor set to `kstack_top - 8` at syscall entry.

`process_fork` copied the parent context and rebased three things into the child kernel stack:

- it read the frame as `process_regs_t`, whose general-register order is the reverse of the actual `push_all` frame, so `tr->rbp` rebased the frame `r8` slot, not `rbp`;
- it rebased `rsp`, which is correct by accident, as `rip`/`cs`/`rflags`/`rsp`/`ss` sit at the same offsets in both layouts;
- it left `r15` alone.

The child kept the parent `r15` anchor, so `mov rsp, [r15 - 16]` read the user `rsp` from the parent kernel stack. After the parent reused that slot for a later syscall, the child picked up the leftover. When the slot held a switch-stub frame the leftover was a RFLAGS constant, which is why the fault address looked like `0x202`.

## Fix

Read the frame as `syscall_regs_t`, which matches the `push_all` layout, and rebase `rsp`, `rbp` and `r15` when each points into the parent kernel stack.

## Related fixes from the same session

- `fix(serial)`: bound the transmit wait in the log path. The kernel log path held a spinlock while polling the UART transmit register; a stalled host blocked it for two seconds. Poll a bounded number of times, then drop the byte.
- `perf(ext2)`: read the image in contiguous runs. Cache the indirect block and coalesce physically contiguous blocks into one block-server read. Cut a 2.2 MiB image load from 4 s to under 1 s.
- `fix(sched)`: reset exec entry registers and signal state in place. Zero the general registers, since the ABI hands the C runtime a stale `%rdx` (the atexit pointer), and clear caught signal dispositions and pending signals.

## Files Changed

| File | Change |
|------|--------|
| `kernel/proc/process.c` | rebase the fork frame anchors with the `syscall_regs_t` layout; rebase `r15` |
| `kernel/arch/x64/serial.c` | bound the transmit wait |
| `userspace/servers/ext2.c` | cache the indirect block; coalesce contiguous reads |
| `kernel/proc/sched.c` | reset exec entry registers and signals; keep the in-place stack out of the old address space |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- Boot on `-smp 2` and `-smp 4`: `selftest PASS`, no faults.
- Mixed command loop, 105 commands, on `-smp 1` and `-smp 4`: no faults. Before the fix, 8.
- Game loop of `tetris` then `q`, 10 times: the prompt returns every time.
- `musltest`: completes (`pipe-ok`, `nanosleep ok`, `mustest: done`).

## Not yet done

- `process_regs_t` still declares the general registers in the reverse order of the saved frame. `process_make` builds the initial frame with it. The registers are zero today, so it stays harmless. Unify the two layouts.
