# 2026-10-31：测试矩阵通过与 shell 重定向

## 变更

- musl：转发 `uname` 与 `signalfd4`；shim 之前对两者返回 ENOSYS。
- proc：in-place exec 后关闭 close-on-exec fd，`cloexectest` 通过。
- shell：启用 `<`、`>`、`>>`。打开文件后 dup 到目标 fd。
- shell：移除 fds 0/1/2 的 TODO；内核单独处理这三个 fd。
- printf：注明内核用 `-mgeneral-regs-only` 构建，格式化保持纯整数。

## 验证

- `-smp 1/2/4`，TCG：`MK: selftest PASS`、`MK: misc PASS`、`MK: signalfd PASS`、`MK: mm PASS`，进 shell。
- `musltest`：`uname HanOS ...`、`signalfd n=128 signo=10`、`getrandom`、`CLOCK_REALTIME`。
- `faulttest: PASS`、`cloexec: child fd closed ok`、`exitgrouptest PASS status=42`、`mem` 打印用量。
- 对宿主 echo 跑 `nettest udp/tcp/srv`：通过。
- shell 套件 `pwd`、`ls`、`cd /bin`、`ls | wc`、`cat`、`wc`；ext2 分区上重定向可用。
- 杀掉 mem 服务后日志打印 `svc: restarting the mem server`，并重新注册 SVC_MM。
