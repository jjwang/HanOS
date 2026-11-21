# 加入 uid/gid、登录与首启密码流程

## 身份

`process_t` 保存 `uid`、`gid`、`umask`。新进程以 root 启动，`umask` 为 022。fork 复制父进程的身份。`getuid`/`getgid`/`geteuid`/`getegid` 返回这些值。`setuid`/`setgid` 允许 root 切换。非 root 只能设置自身。`umask` 保存创建掩码。musl shim 转发这些调用，不再把每个进程固定成 root。

## 密码存储

本树里 `libcrypt` 为空归档。构建自带 SHA-256。存储格式为 `$5$<salt>$<sha256(salt||password)>`。shadow 放在 ext2 数据分区的 `/data/etc/shadow`。首启设置的密码重启后保留。`build-hdd-image.sh` 预置 `root::` 与 `kite::` 空哈希。

## 登录

`/bin/login` 从 `/etc/passwd` 读 uid、gid 与 shell，从 shadow 读哈希。哈希为空时强制输入两次新密码并写回。否则最多校验三次。成功后调用 `setgid`/`setuid`，选择可执行的 shell 并 exec。`/bin/passwd` 修改当前用户密码。`init` 启动 `/bin/login` 代替 shell。

## 验证

QEMU 中登录走到 `pw_lookup`。QEMU monitor 的脚本按键会打乱用户名，仍需真实键盘交互测试。

## 待办

- `chown`/`fchownat`、`open` 模式位检查；创建文件时应用 `umask`。
