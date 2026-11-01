# Linux 移植包与 bash

## 构建选项

`make ports` 构建可选 Linux 包。
该命令重新生成 `cdrom.iso` 与 `release/hdd.img`。
普通 `make` 不包含这些包。
构建把源码下载到 `3rdparty/build/ports/dl`。
每个包链接 HanOS musl。
二进制暂存到 `3rdparty/build/ports/root/bin`。

## 包列表

| 包 | 版本 |
| --- | --- |
| bash | 5.2.37 |
| coreutils | 9.5 |
| grep | 3.11 |
| sed | 4.9 |
| tar | 1.35 |

## 镜像布局

- `cdrom.iso`：包进入 initrd 的 `/bin`。拷贝跳过 HanOS 服务名（`tty`、`input`、`vfs` 等）。coreutils 的 `tty` 不覆盖 tty 服务。
- `release/hdd.img`：包进入 `/data/bin`，与游戏同目录。initrd 只放基础用户态。
- `/root/.bashrc` 设置 `PATH=/data/bin:/bin:/usr/bin` 与 `TERM=hanos`。

## 移植要点

- `3rdparty/ports/cc` 把 musl 头文件路径放在包参数之后。包自带的替代头文件优先。
- 包装器加 `-fcommon` 与 `-z max-page-size=0x2000`。`-fcommon` 保留 coreutils 的 termcap 全局变量。页大小去掉每个可执行文件 2 MB 的空隙。
- bash 用 `--disable-readline` 构建。readline 把 tty 切到非规范模式。首次读取返回 EOF。
- 进程服务保留服务端打开文件描述。最后一个复制 fd 关闭时释放。bash 每次重定向都先 `dup2` 再 `close`。
- `fcntl` 处理 `F_DUPFD` 与 `F_DUPFD_CLOEXEC`。标准描述符按有效处理。tar 启动前检查 fd 0..2。
- `getppid` 返回记录的父进程号。

## 限制

- 作业控制关闭。内核不投递信号。内核没有进程组。
- 写入端为 coreutils `ls` 的管道不完成。写入端为基础 `/bin/ls` 的管道完成。
