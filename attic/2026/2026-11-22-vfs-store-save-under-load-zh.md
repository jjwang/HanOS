# 负载下持续推进 VFS store 保存

## 问题

`/data/etc/shadow` 的 root 哈希重启后丢失，login 又要求设密。VFS 把可写文件打包进 ext2 上的 `/.vfs-store`，分步保存。该步骤只在 recv 超时（服务端空闲）时执行。登录、bash、exec 一直在读文件，保存长期跑不完。磁盘上仍是旧 store，重启就丢。

## 修复

主循环每轮都推进 `store_save_step`，不再只在空闲时。写入在随后几次请求内落盘。

## 验证

设置 root 密码后，同一镜像连续重启多次，login 显示 `Password:` 而不是要求重设。

## 遗留

保存仍在原地截断 `/.vfs-store`，中途被打断会留下残缺文件。原子写（临时文件加 rename）需要 ext2 rename，ext2 服务端暂未提供。
