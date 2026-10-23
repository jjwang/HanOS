# 2026-10-23：FAT32 写入与 VFS 持久化

## 改动

- block：经 `BLOCK_GET_PART` 挂载 FAT 分区 0。保留 AHCI SATA 路径。删去 ATA PIO 表述。
- fat32：新增 `FAT_CREATE`、`FAT_WRITE`、`FAT_TRUNC` 与 `FAT_UNLINK`。分配簇、连接簇链，同步所有 FAT 副本。
- fat32：目录游标留在服务。内核临时 fd 不再丢失 `readdir` 位置。
- fat32：写入接受 8.3 名。读取仍支持长名。
- vfs：内核把 ext2 endpoint 授予 VFS 服务。运行时文件存入分区 1 的 `/.vfs-store`。
- vfs：启动时读回存储。create、write、truncate、unlink 与 rename 后写回。
- kernel：`/fat` 的 create、write、truncate 与 unlink 转到 FAT 服务。
- build：EFI 系统分区改为 FAT32。
- block：等探测线程结束再启动 init。磁盘摘要先于 shell 打印。
