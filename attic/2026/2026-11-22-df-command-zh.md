# 新增 df 命令

`bin/df` 调用 `statfs("/data")`，打印 ext2 分区的总量、已用与可用 KiB。statfs 系统调用与 ext2 statfs 路径已存在，本次只加命令与 `CELF` 条目。
