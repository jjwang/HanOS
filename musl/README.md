# musl on HanOS

HanOS userspace links against musl. The kernel does not.

## Layout

```
musl/syscall_arch.h         HanOS syscall backend for musl
musl/__set_thread_area.s    TLS setup through SYSCALL_SET_FS_BASE
musl/linker.ld              userspace link script
musl/build.sh               copies the overrides and builds musl
3rdparty/musl/              vendored musl source (not tracked)
```

## Vendor the source

Fetch musl 1.2.5 and unpack it into `3rdparty/musl`:

```
mkdir -p 3rdparty/musl
curl -L https://musl.libc.org/releases/musl-1.2.5.tar.gz | tar xz -C 3rdparty/musl --strip-components=1
```

## Build

```
make -C userspace musl
```

The userspace build runs `musl/build.sh` on demand. To build alone:

```
musl/build.sh
```

`musl/build.sh` copies `syscall_arch.h` and `__set_thread_area.s` into the source tree, then runs:

```
./configure --target=x86_64-hanos --disable-shared --enable-static \
    CC=x86_64-elf-gcc CFLAGS='-mcmodel=large -mno-red-zone'
make AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib
```

Output is `3rdparty/musl/lib/libc.a` plus the `crt` objects.

## Port notes

- Syscalls follow the Linux x86-64 ABI. `include/syscall_nr.h` defines the numbers for both kernel and userspace. POSIX calls keep their Linux numbers; HanOS-only calls use `0x400` and above.
- `syscall_arch.h` keeps the POSIX calls on their Linux numbers and maps the HanOS-only calls. It translates the `statx`, `getdents64`, `fcntl`, `readv`, and `writev` requests to the HanOS handlers.
- `__set_thread_area.s` issues `SYSCALL_SET_FS_BASE` (`0x401`). The raw `arch_prctl` instruction faults.
- `linker.ld` loads userspace at `0x0000400000000000` and defines `_DYNAMIC` for musl startup.
- `-mno-red-zone` is mandatory. The HanOS syscall handler runs on the user stack, so the kernel frame clobbers the 128-byte red zone. Without the flag, a large local such as a `readdir` buffer returns corrupted data.

## Unsupported

- Threads: `clone(CLONE_VM)` returns `ENOSYS`. musl `pthread_create` fails.
- Signals: the handlers exist but do not deliver.
