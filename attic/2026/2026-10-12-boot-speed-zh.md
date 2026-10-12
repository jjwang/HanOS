# 2026-10-12: 启动与 exec 加速

## 现象

内核从块服务器到 shell 用时 6.5 s。
`SCHED: execute "/bin/hansh"` 到 `k_execve: inplace` 单独用时 2069 ms。
早期服务器每个用时 110 ms。

## 背景

启动路径用 `sched_execve` 启动每个服务器。
它调用 `elf_load` 与 `vfs_load_file`。
链接器把单个加载段对齐到 2 MB 文件偏移。
每个 userspace 映像为 2.2 MB。
读取会拷贝整个文件。

## 根因

### 1. VFS 服务器提供 initrd

VFS 服务器注册后，`vfs_load_file` 选 `vfs_load_via_server`。
服务器映射 initrd，经 IPC 与内存对象返回每个文件。
读取 2.2 MB 映像耗时 2069 ms。
每次后续 exec 都付出这个代价。
早期服务器在 VFS 服务器之前启动，直接读 initrd，保持很快。

### 2. 启动探测做多余工作

`kShell` 用读写与 stat 自检探测 fat32、ext2、vfs、process。
`block_server_probe` 遍历 GPT、读扇区、做写读自检。

## 实现

### 直接读 initrd (00a29e4)

- `vfs_load_file` 先试 `initrd_load`，命中即返回其缓冲。
- VFS 服务器只提供 ext2 卷等路径。

### 减少启动探测 (9549326)

- 从 `kShell` 去掉 fat32、ext2、vfs、process 探测。
- `block_server_probe` 发一次 `BLOCK_GET_INFO`，磁盘型号照旧打印。

## 改动文件

| 文件 | 改动 |
|------|------|
| `kernel/fs/vfs.c` | 在 VFS 服务器之前读 initrd 文件 |
| `kernel/kmain.c` | 去掉 fat32、ext2、vfs、process 探测 |
| `kernel/srv/block_srv.c` | `block_server_probe` 减为一次 GET_INFO |

## 测试

- `make -C kernel clean && make -C kernel` 无警告。
- `-smp 2` 与 `-smp 4` 启动：无 panic、exception、corrupt 行。
- `SCHED: execute "/bin/hansh"` 到 `k_execve: inplace` 从 2069 ms 降到 184 ms；块到 shell 链从 6.5 s 降到 1.7 s。
- 交互 `ls`、`pwd` 运行。
- `snake` 从 `/data/bin` 加载并以 0 退出。

## 未完成

- userspace 链接器把每个加载段对齐到 2 MB 文件偏移。
- 映像与 initrd 各有 2 MB 填充；initrd 为 60 MB。
- `-z max-page-size=4096` 把 2.2 MB 映像缩到 84 KB，initrd 缩到 5 MB。
- 但 console 服务器随后在 `vterm_allocator_malloc` 触发 fault。
- 该 fault 仅出现在 `-smp 4`。
- 先找到该 fault，再缩小映像。
- 剩余单次 exec 开销在进程创建与调度，`ls` 为 180 ms。
