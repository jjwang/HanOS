# 修复 ls 别名与 mem 命令

## ls

`.bashrc` 给 `ls`、`grep` 加了 `--color=auto` 别名。移植的 coreutils 不接受该选项，打印多行 `invalid option`。去掉别名里的该选项。

## mem

`/bin/mem` 是 MM 服务端。输入 `mem` 会启动它并卡在 bootinfo 循环。把服务端改名为 `/bin/mm`（同步 `kconfig` 与 MM 服务启动），并新增 `bin/mem` 命令输出内存统计。

## 验证

guest shell 里 `ls` 不再打印 `invalid option`，`mem` 输出内存统计。
