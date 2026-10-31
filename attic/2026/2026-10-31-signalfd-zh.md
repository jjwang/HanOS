# 2026-10-31：signalfd

## 变更

- signal：新增 `SVC_SIGNALFD` 与内核 signalfd 对象表。
- signal：新增 `signalfd4`。新 fd 保存掩码，已有 fd 更新掩码。
- signal：读取时取出 fd 掩码与进程掩码共同选中的待决信号，返回 `signalfd_siginfo`。
- signal：掩码内、已阻塞的待决信号就绪时，`poll` 报 POLLIN。
- syscall：新增 `SYSCALL_SIGNALFD4` 与表项。
- kmain：服务启动后运行 `signalfd_selftest`，打印 `MK: signalfd PASS`。
- ci：要求 `MK: signalfd PASS`。
- musltest：阻塞 SIGUSR1，发出后读取 signalfd。

## 验证

- GCC 13.1 与交叉 GCC 10.2，TCG，`-smp 1`：`MK: selftest PASS`，`MK: misc PASS`，`MK: signalfd PASS`，shell 以 `hansh` 启动。
