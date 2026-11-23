# Flush the TLB when a reused CR3 does not change

## Problem

About one in 27 one-GiB boots killed the ext2 server with a page fault at the anon mmap base (`0x80000000000`) or the brk base (`0x400100001098`), and the service monitor then restarted it, delaying `/data`. The page-fault error code reported the page present while the page table walk showed it missing: a stale TLB entry.

The anon and brk bases are the same virtual addresses for every process, and a freed address space's PML4 physical page is reused for the next process, so the two carry the same CR3 value. A MOV to CR3 with an unchanged value need not invalidate the TLB, so the new process read a dead process's stale mapping at those addresses.

## Fix

On a context switch, compare the outgoing and incoming CR3. When they are equal, toggle CR4.PGE (write the flipped value, then the original) to invalidate the whole TLB before the switch. A changed CR3 still flushes on the MOV.

## Verification

45 one-GiB boots and two stress runs complete with no page fault. Before the fix the fault appeared about once in 27 boots at the same memory size.
