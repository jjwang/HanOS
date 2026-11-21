# 实施文件权限与按包 ports stamp

## 权限

`chown`/`fchownat` 经 `VFS_CHOWN`、`EXT2_CHOWN` 到 ext2 服务端。内核 `k_chown` 只允许 root，其他进程得到 `EPERM`。非 root 不能改属主。`vfs_open_path` 按属主/属组/其他三段模式位检查调用者的读写请求。root 跳过检查。创建文件时先应用 `0666 & ~umask`。文件继承进程记录的身份与 umask。

## Ports

`3rdparty/ports/build.sh` 为每个包保留一个 stamp（`.pkg-<name>`）。包的脚本、编译器包装器或 `common.sh` 更新时才重建。暂存目录保留，未变的包留在原处。整轮构建跳过已完成的包，不再重编 bash、readline 等。

## 待办

- initrd 的 VFS 命名空间接受 `chown` 但不保存属主；只有 ext2 文件持久保存属主。
- 登录提示未刷到串口；仍需真实键盘交互测试。
