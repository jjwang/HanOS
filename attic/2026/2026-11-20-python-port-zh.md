# 2026-11-20：移植 CPython 3.8 并使用共享模块

- 用 HanOS 的 musl sysroot 构建 CPython 3.8.18。配置开启 `--enable-shared`。
- 解释器动态链接 `libpython3.8.so` 与共享 musl libc。
- 扩展模块构建为共享对象。x86_64-elf 驱动丢弃 `-shared`。ports 包装器改为传 `-Wl,-shared`。
- 修正 `getrandom` 返回字节数。CPython 的哈希种子循环等待缓冲区填满。
- `libpython3.8.so` 放入 ports 的 initrd。标准库与 `lib-dynload` 放在 ext2 的 `lib/python3.8`。
- 验证：`python3 -c "print(40+2)"` 输出 `42`。
