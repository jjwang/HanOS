# 解耦匿名 mmap 地址，存档移出请求路径

## 匿名 mmap 虚拟地址

内核用物理地址推导匿名映射的虚拟地址（`phys_ptr + 0x80000000000`）。两个物理块相差一个页表跨度时落入同一 PTE 槽。后一次 `map_page` 覆盖前一次映射。活动映射指向另一分配的物理页。现象：新 server 首个 `sys_malloc` 偶发失败，数据静默损坏。

`addrspace_t` 增加 `anon_next`。非固定映射时 `k_vm_map` 按映射长度推进该游标。`vma_clone` 把高水位复制给子地址空间。物理地址与虚拟地址就此解耦。

## 存档

VFS 在空闲路径同步执行完整 `store_save`。每个 ext2 块是一条独立 AHCI 命令。QEMU 下每 4 KiB 约 0.4 秒。60 KiB 存档占用 VFS 约 6 秒。内核 `vfs_open`（ELF 加载）与 `k_pipe` 使用 1 秒 `router_forward` 超时。存档期间的请求失败：

- `VFS: open "/root/stress_bin" failed`，随后 `File header error`，跳到地址 0。
- `bash: start_pipeline: pgrp pipe: Out of memory`。

两处改动：

- ext2 `file_write` 合并物理连续整块。每段一条 AHCI 命令。
- `store_save` 改为状态机：`create`、`trunc`、`header`，之后每次空闲处理一个条目头或一个数据块。每步回到主循环。请求最多等待一步。

`unlink` 只置脏标记，不再内联存档。

## 测试脚本

`scripts/stress-test.sh` 把逐键注入间隔从 0.16 秒提到 0.22 秒。丢键会把命令截断成 `/hz`，导致测试失败。

## 验证

`make stress` 连续四轮通过。日志无 `start_pipeline`、`VFS: open ... failed`、page fault、panic 行。
