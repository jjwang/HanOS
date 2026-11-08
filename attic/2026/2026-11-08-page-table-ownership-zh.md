# 2026-11-08：页表归属与 mem_list 改名

## 背景

`addrspace_t.mem_list` 记录页表块归属。
归属对象为地址空间。
每个块由 `pmm_get(8)` 分配。
它返回 8 个物理页。
`unmap_page()` 清 PTE 后回收空表。
回收对象包括 PT、PD 与 PDPT。
对应块同时从列表删除。
block 服务每次传输都映射 `0x20000000`。
ext2 bounce 每次传输都映射 `0x30000000`。
旧路径在每次解映射后把表页还给 PMM。
CPU 的 TLB 与 paging-structure cache 可能保留旧翻译。
PMM 把这些物理页交给另一个映射。
下一次传输走到旧表。
页故障落在 `0x20000000` 或 `0x30000000`。

## 修改

- `mem_list` 改名为 `page_tables`。向量保存每个 8 页表块的物理基址。
- `unmap_page()` 只清 PTE 并失效翻译。表块保留在 `page_tables`。
- `process_free_addrspace()` 先执行 `vma_destroy()` 与 `addrspace_owned_destroy()`。
- 它随后逐块释放 `page_tables`。
- `alloc_table()` 与 `clone_table()` 把新块加入 `page_tables`。
- `table_owned()` 扫描这个向量，区分共享内核分支与复制表。
- IPC selftest 地址空间通过同一归属列表释放 `page_tables`。

## 后果

- 页表存续到地址空间退出。解映射不再把表页还给 PMM。
- 进程触及多个 2 MB 区域时，每个区域保留一张表，直到进程退出。
- 长期服务在同一地址反复映射，复用同一张表。
- `process_free_addrspace()` 仍是唯一释放点。
- 它释放表块、`owned_list` 内核缓冲与 PML4。

## 验证

- 80 次 fork/exec 通过。旧的 `map_page` 与 `next_table` 故障不再出现。
- 6 轮并发 `du`、`cut`、`md5sum`、`sed` 通过。无页故障，无 ext2 重启。
- 内核 clean build 无警告。基础镜像与 ports 镜像启动测试通过。

提交：`1e12d23 fix(mm): keep page tables until address space teardown`。
