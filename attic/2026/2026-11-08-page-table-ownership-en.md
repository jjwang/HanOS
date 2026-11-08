# 2026-11-08: Page table ownership and the mem_list rename

## Background

`addrspace_t.mem_list` tracked the page-table blocks owned by an address space. Each block comes from `pmm_get(8)`, which returns eight physical pages. `unmap_page()` cleared the PTE, then freed an empty PT, PD or PDPT and removed its block from the list.

The block server and the ext2 bounce buffer map and unmap `0x20000000` and `0x30000000` for every transfer. The old path returned those table pages to PMM after each unmap. The CPU can retain TLB entries and paging-structure cache entries for the removed translations. PMM can then hand the same physical page to another mapping. The next transfer walked a stale table and faulted at `0x20000000` or `0x30000000`.

## Change

- Rename `mem_list` to `page_tables`. The vector stores the physical base of each eight-page table block.
- `unmap_page()` clears the PTE and invalidates the translation. The table block stays in `page_tables`.
- `process_free_addrspace()` walks `page_tables` and releases every block after `vma_destroy()` and `addrspace_owned_destroy()`.
- `alloc_table()` and `clone_table()` append each new block to `page_tables`. `table_owned()` scans the vector to decide between a shared kernel branch and a clone.
- The IPC selftest address space frees `page_tables` through the same ownership list.

## Consequences

- Page tables live until the address space exits. An unmap no longer returns table pages to PMM.
- A process that touches many 2 MB regions keeps one table per region until exit. Long-lived servers reuse the same table for repeated maps at a fixed address.
- `process_free_addrspace()` remains the single release point. It frees the table blocks, the `owned_list` kernel buffers and the PML4.

## Verification

- 80 fork/exec cycles completed without the old `map_page` or `next_table` fault.
- Six rounds of concurrent `du`, `cut`, `md5sum` and `sed` completed with no page fault and no ext2 restart.
- Kernel clean build passed without warnings. Base and ports boot tests passed.

Commit: `1e12d23 fix(mm): keep page tables until address space teardown`.
