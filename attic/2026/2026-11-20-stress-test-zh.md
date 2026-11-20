# 2026-11-20：新增标准压测

- 新增 `scripts/stress-test.sh` 与 `scripts/guest-stress.sh`。
- `make stress` 重建 `release/hdd.img WITH_PORTS=1`，在私有副本上运行 guest 测试。
- guest 执行 `ls`、`free`、`cmp`、Python 与 C++ 编译。guest 失败或内核/加载器报错即判失败。
- 保护 native 工具链拷贝。基础镜像无自举工具链也能构建。
- 测试让 VFS 文件描述符表按需增长。端口镜像上暴露了运行时文件写入失败。
