#!/bin/bash
# Install the musl headers, libraries and startup objects into a target sysroot.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MUSL="$ROOT/3rdparty/build/musl"
SHARED="$ROOT/3rdparty/build/musl-shared/lib/libc.so"
STAGE="$ROOT/3rdparty/build/sysroot"

rm -rf "$STAGE"
mkdir -p "$STAGE/usr/include" "$STAGE/usr/lib"

cp -a "$MUSL/include/." "$STAGE/usr/include/"
cp -a "$MUSL/arch/x86_64/." "$STAGE/usr/include/"
cp -a "$MUSL/arch/generic/." "$STAGE/usr/include/"
cp -a "$MUSL/obj/include/." "$STAGE/usr/include/"

cp -f "$MUSL/lib/libc.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/libm.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/libdl.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/libpthread.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/librt.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/libcrypt.a" "$STAGE/usr/lib/"
cp -f "$MUSL/lib/libresolv.a" "$STAGE/usr/lib/"
cp -f "$MUSL/obj/crt/crt1.o" "$STAGE/usr/lib/"
cp -f "$MUSL/obj/crt/x86_64/crti.o" "$STAGE/usr/lib/"
cp -f "$MUSL/obj/crt/x86_64/crtn.o" "$STAGE/usr/lib/"
if [ -f "$SHARED" ]; then
    cp -f "$SHARED" "$STAGE/usr/lib/libc.so"
fi

touch "$STAGE/.installed"
