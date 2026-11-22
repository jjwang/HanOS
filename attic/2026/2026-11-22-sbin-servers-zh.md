# 服务端迁移到 /sbin

ring-3 服务端原先放在 `/bin`。在 shell 输入同名命令（早先 `mm`、`mem`）会启动服务端并卡在服务循环。把服务端放 `/sbin`，命令留 `/bin`；`/sbin` 不在 shell 的 PATH。`kernel/kconfig.h` 的 `DEFAULT_*_SVR`、用户态 Makefile 与 initrd 打包同步改为 `/sbin`。
