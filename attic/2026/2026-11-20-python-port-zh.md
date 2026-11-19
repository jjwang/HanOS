# 2026-11-20：移植 CPython 3.8

- 用 HanOS 的 musl sysroot 构建静态 CPython 3.8.18。
- x86_64-elf 工具链不能生成共享对象。扩展模块从 `Modules/Setup.local` 静态链接。
- 去掉依赖 OpenSSL、libffi、expat、zlib 或数据库的模块。
- 标准库放到 ext2 的 `lib/python3.8`。镜像复制 ports 的 `lib` 目录。
- ISO 的 initrd 跳过解释器。live 镜像没有数据分区。
- 验证：`python3 -V` 输出 `3.8.18`，`python3 -c "print(40+2)"` 输出 `42`。
