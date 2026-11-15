#!/bin/bash
# Build binutils binaries that run on HanOS.
set -e

VERSION=2.42
ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC="$ROOT/3rdparty/build/toolchain/src/binutils-$VERSION"
BUILD="$ROOT/3rdparty/build/toolchain/build-native-binutils"
PREFIX="$ROOT/3rdparty/build/toolchain/native"
PORTS="$ROOT/3rdparty/ports"
MUSL="$ROOT/3rdparty/build/musl"

[ -d "$SRC" ] || {
    echo "run scripts/build-binutils.sh first" >&2
    exit 1
}

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
CC="$PORTS/cc" AR=x86_64-elf-ar RANLIB=x86_64-elf-ranlib \
    CFLAGS="-O2 -fcommon" \
    "$SRC/configure" \
    --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-musl \
    --target=x86_64-hanos --prefix="$PREFIX" \
    --disable-nls --disable-werror --disable-shared --enable-static \
    --disable-gdb --disable-sim --disable-gprof --disable-gold \
    --disable-multilib --disable-werror
make -j"$(nproc)" MAKEINFO=true
make install MAKEINFO=true

echo "native binutils: $PREFIX/bin"
