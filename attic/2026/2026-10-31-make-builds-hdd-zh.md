# 2026-10-31：make 同时生成磁盘镜像

## 变更

- build：`all` 同时生成 `cdrom.iso` 与 `release/hdd.img`。
- build：两个镜像共用 `initrd.tar` 目标，并行 make 不会争用归档。
- docs：更新 README 的构建说明。

## 验证

- GCC 10.2，TCG：`make` 生成 `cdrom.iso` 与 `release/hdd.img`。两者都启动到 `hansh`，打印 `MK: selftest PASS`、`MK: misc PASS`、`MK: signalfd PASS`。
