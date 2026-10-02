# HanOS 上的 musl

HanOS 用户态链接 musl。内核不链接 musl。

## 目录

```
musl/syscall_arch.h         musl 的 HanOS 系统调用后端
musl/__set_thread_area.s    用 SYSCALL_SET_FS_BASE 建立 TLS
musl/linker.ld              用户态链接脚本
musl/build.sh               拷贝覆盖文件并编译 musl
3rdparty/musl/              存放 musl 源码（不入库）
```

## 准备源码

取 musl 1.2.5，解包到 `3rdparty/musl`：

```
mkdir -p 3rdparty/musl
curl -L https://musl.libc.org/releases/musl-1.2.5.tar.gz | tar xz -C 3rdparty/musl --strip-components=1
```

## 编译

```
make -C userspace musl
```

用户态构建会自动调用 `musl/build.sh`。单独构建：

```
musl/build.sh
```

`musl/build.sh` 先把 `syscall_arch.h` 和 `__set_thread_area.s` 拷进源码树，再执行：

```
./configure --target=x86_64-hanos --disable-shared --enable-static \
    CC=x86_64-elf-gcc CFLAGS='-mcmodel=large -mno-red-zone'
make AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib
```

输出为 `3rdparty/musl/lib/libc.a` 和 `crt` 目标文件。

## 移植要点

- 系统调用遵循 Linux x86-64 ABI。`include/syscall_nr.h` 为内核和用户态定义号。POSIX 调用沿用 Linux 号，HanOS 独有调用用 `0x400` 以上。
- `syscall_arch.h` 让 POSIX 调用直接沿用 Linux 号，只映射 HanOS 独有调用。它把 `statx`、`getdents64`、`fcntl`、`readv`、`writev` 请求转成 HanOS 处理函数。
- `__set_thread_area.s` 发起 `SYSCALL_SET_FS_BASE`（`0x401`）。原始的 `arch_prctl` 会触发异常。
- `linker.ld` 把用户态加载到 `0x0000400000000000`，并为 musl 启动定义 `_DYNAMIC`。
- 必须加 `-mno-red-zone`。HanOS 的系统调用处理函数跑在用户栈上，内核栈帧会覆盖 128 字节红区。不加该标志时，`readdir` 这类大局部变量会读到被破坏的数据。

## 尚不支持

- 线程：`clone(CLONE_VM)` 返回 `ENOSYS`，musl 的 `pthread_create` 失败。
- 信号：处理函数已定义，但不投递。
