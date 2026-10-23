# 2026-10-23：系统调用返回时保留 GS 基址

## 现象

QEMU 4.2 用 TCG 启动时，console 进程第一次 `syscall` 触发缺页。故障在 `syscall_handler`，地址 0x10，早于内核栈切换。同一镜像在 KVM 下正常。

## 原因

TCG 的用户返回路径把 `GS.base` 留成 0。处理函数没有先交换 GS，就直接访问每 CPU 槽位。SYSRET 后 `KERN_GS_BASE` 也可能丢失，下一次系统调用仍读到 0。

## 修复

- 系统调用入口先交换 GS，再访问每 CPU 槽位。
- SYSRET 前把当前内核 `GS.base` 写回 `KERN_GS_BASE`，再交换回用户 GS。
- 返回路径仍用 SYSRET。

同一镜像现在能在 QEMU 4.2 TCG 与 KVM 下启动。无头 TCG 测试能执行 `pwd`。
