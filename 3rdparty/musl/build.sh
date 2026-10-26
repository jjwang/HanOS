#!/bin/bash
#
# Build musl for HanOS in 3rdparty/build/musl.
#
# The vendored source is downloaded into 3rdparty/build/musl. The HanOS port
# files stay in this directory.
#
set -e

OVERLAY=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$OVERLAY/../.." && pwd)
BUILD="$ROOT/3rdparty/build/musl"
CC=${CC:-x86_64-elf-gcc}
AR=${AR:-x86_64-elf-ar}
RANLIB=${RANLIB:-x86_64-elf-ranlib}

if [ ! -f "$BUILD/src/stdio/printf.c" ]; then
    tmp=$(mktemp)
    trap 'rm -f "$tmp"' EXIT
    ok=0
    for attempt in 1 2 3 4 5; do
        if curl -fL --retry 5 --retry-delay 2 --retry-connrefused \
            -o "$tmp" https://musl.libc.org/releases/musl-1.2.5.tar.gz; then
            ok=1
            break
        fi
        sleep $((attempt * 2))
    done
    [ "$ok" -eq 1 ]
    tar -tzf "$tmp" >/dev/null
    rm -rf "$BUILD"
    mkdir -p "$BUILD"
    tar -xzf "$tmp" -C "$BUILD" --strip-components=1
    test -f "$BUILD/src/stdio/printf.c"
    test -f "$BUILD/configure"
    test -f "$BUILD/Makefile"
fi

cp "$OVERLAY/syscall_arch.h" "$BUILD/arch/x86_64/syscall_arch.h"
cp "$OVERLAY/__set_thread_area.s" \
   "$BUILD/src/thread/x86_64/__set_thread_area.s"

cd "$BUILD"
make distclean >/dev/null 2>&1 || true
./configure --target=x86_64-hanos --disable-shared --enable-static \
    CC="$CC" CFLAGS='-mcmodel=large -mno-red-zone -Wno-return-local-addr'
make AR="$AR" RANLIB="$RANLIB" -j"$(nproc)"

echo "musl: $BUILD/lib/libc.a"
