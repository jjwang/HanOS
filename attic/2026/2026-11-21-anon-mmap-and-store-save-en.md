# Decouple anonymous mmap addresses and keep store saves off the request path

## Anonymous mmap virtual address

The kernel derived an anonymous mapping's virtual address from its physical
address (`phys_ptr + 0x80000000000`). Two chunks a page-table span apart landed
in the same PTE slot. The later `map_page` overwrote the earlier mapping, so a
live mapping pointed at another allocation's pages. Symptoms: an intermittent
first `sys_malloc` failure in a fresh server, and silent data corruption.

`addrspace_t` gains `anon_next`. `k_vm_map` advances it by the mapping size for
non-fixed mappings. `vma_clone` copies the high-water mark into the child
address space. Physical and virtual addresses are now independent.

## Store save

The VFS `store_save` ran the whole store synchronously from the idle path.
Each ext2 block was a separate AHCI command, about 0.4 s per 4 KiB under QEMU. A
60 KiB save held the VFS for about 6 s. Kernel `vfs_open` (the ELF loader) and
`k_pipe` use a 1 s `router_forward` timeout, so requests that arrived during a
save failed:

- `VFS: open "/root/stress_bin" failed`, then `File header error`, then a jump
  to address 0.
- `bash: start_pipeline: pgrp pipe: Out of memory`.

Two changes:

- ext2 `file_write` merges physically contiguous full blocks into one block
  server request, one AHCI command per run instead of one per block.
- `store_save` becomes a state machine: `create`, `trunc`, `header`, then one
  entry header or one data chunk per idle iteration. Each step returns to the
  main loop, so a request waits at most one step.

`unlink` marks the store dirty instead of saving inline.

## Test harness

`scripts/stress-test.sh` raises the per-key injection interval from 0.16 s to
0.22 s. A dropped keystroke truncated the command to `/hz` and failed the run.

## Verification

`make stress` passes four consecutive runs. The log has no `start_pipeline`,
`VFS: open ... failed`, page fault, or panic line.
