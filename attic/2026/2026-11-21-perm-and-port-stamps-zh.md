# 实施文件权限与按包 ports stamp

## 权限

`chown`/`fchownat` 经 `VFS_CHOWN`、`EXT2_CHOWN` 到 ext2 服务端。内核 `k_chown` 只允许 root，其他进程得到 `EPERM`。非 root 不能改属主。`vfs_open_path` 按属主/属组/其他三段模式位检查调用者的读写请求。root 跳过检查。创建文件时先应用 `0666 & ~umask`。文件继承进程记录的身份与 umask。

## Ports

`3rdparty/ports/build.sh` 为每个包保留一个 stamp（`.pkg-<name>`）。包的脚本、编译器包装器或 `common.sh` 更新时才重建。暂存目录保留，未变的包留在原处。整轮构建跳过已完成的包，不再重编 bash、readline 等。

## 验证

同一镜像两次启动：首次设密码，第二次用密码登录进入 shell。initrd 的 VFS 条目与 store 第 3 版保存 uid/gid，chown 可持久化。登录提示用 write 输出到串口，密码从 stdin 读取。
