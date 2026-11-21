# 2026-11-20：新增标准压测

- 新增 `scripts/stress-test.sh` 与 `scripts/guest-stress.sh`。
- `make stress` 重建 `release/hdd.img WITH_PORTS=1`，在私有副本上运行 guest 测试。
- guest 执行 `ls`、`free`、`cmp`、Python 与 C++ 编译。guest 失败或内核/加载器报错即判失败。
- 保护 native 工具链拷贝。基础镜像无自举工具链也能构建。
- VFS、ext2、进程、pipe 与 end 表改为按需增长。期间发现的 VFS open 卡死已修。
- dup 替换描述符前刷新 VFS 写缓存。open 路径不再同步写 store。重定向与编译器临时文件正常。
- ports 镜像通过压测。
- 运行时存储在内核空闲时落盘。运行时文件可持久化，且不阻塞请求。
- store 记录文件 mode。chmod 的修改重启后仍在。
- store 改为流式写入每条记录。读取上限 1 MiB。大文件受 ext2 写吞吐限制。
