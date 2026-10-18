# musl on HanOS

HanOS userspace links against musl. The kernel does not.

## Layout

```
3rdparty/musl/syscall_arch.h       HanOS syscall backend for musl
3rdparty/musl/__set_thread_area.s  TLS setup through SYSCALL_SET_FS_BASE
3rdparty/musl/linker.ld            userspace link script
3rdparty/musl/build.sh             downloads and builds musl
3rdparty/build/musl/               source and build output (not tracked)
```

## Vendor the source

`3rdparty/musl/build.sh` downloads musl 1.2.5 into `3rdparty/build/musl` when
the source is missing. To vendor manually:

```
mkdir -p 3rdparty/build
curl -L https://musl.libc.org/releases/musl-1.2.5.tar.gz \
    | tar xz -C 3rdparty/build/musl --strip-components=1
```

## Build

```
make -C userspace musl
```

The userspace build runs `3rdparty/musl/build.sh` on demand. To build alone:

```
3rdparty/musl/build.sh
```

The script downloads the source when needed, copies `syscall_arch.h` and
`__set_thread_area.s` into the build tree, then runs:

```
./configure --target=x86_64-hanos --disable-shared --enable-static \
    CC=x86_64-elf-gcc CFLAGS='-mcmodel=large -mno-red-zone -Wno-return-local-addr'
make AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib
```

Output is `3rdparty/build/musl/lib/libc.a` plus the `crt` objects.

## Port notes

- Syscalls follow the Linux x86-64 ABI. `include/syscall_nr.h` defines the numbers for both kernel and userspace. POSIX calls keep their Linux numbers; HanOS-only calls use `0x400` and above.
- `syscall_arch.h` keeps the POSIX calls on their Linux numbers and maps the HanOS-only calls. It translates the `statx`, `getdents64`, `fcntl`, `readv`, and `writev` requests to the HanOS handlers.
- `__set_thread_area.s` issues `SYSCALL_SET_FS_BASE` (`0x401`). The raw `arch_prctl` instruction faults.
- `linker.ld` loads userspace at `0x0000400000000000` and defines `_DYNAMIC` for musl startup.
- `-mno-red-zone` is mandatory. The HanOS syscall handler runs on the user stack, so the kernel frame clobbers the 128-byte red zone. Without the flag, a large local such as a `readdir` buffer returns corrupted data.

## Unsupported

- Threads: `clone(CLONE_VM)` returns `ENOSYS`. musl `pthread_create` fails.
- Signals: the handlers exist but do not deliver.
