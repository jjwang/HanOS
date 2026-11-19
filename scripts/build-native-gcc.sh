#!/bin/bash
# Build GCC and its C/C++ frontends to run on x86_64-hanos.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/3rdparty/build/toolchain"
SRC="$TOOL/src/gcc-10.2.0"
BUILD="$TOOL/build-native-gcc"
PREFIX="$TOOL/native-gcc"
SYS="$ROOT/3rdparty/build/sysroot"
CROSS="$TOOL/x86_64-hanos/bin"

[ -d "$SRC" ] || {
    echo "run scripts/build-gcc.sh first" >&2
    exit 1
}
[ -x "$CROSS/x86_64-hanos-g++" ] || {
    echo "x86_64-hanos-g++ missing" >&2
    exit 1
}

for lib in libgmp.a libmpfr.a libmpc.a; do
    if [ ! -f "$SYS/usr/lib/$lib" ]; then
        "$ROOT/scripts/build-math-libs.sh"
        break
    fi
done

rm -rf "$BUILD"
mkdir -p "$BUILD/tools"
ln -sf "$CROSS/x86_64-hanos-as" "$BUILD/tools/as"
ln -sf "$CROSS/x86_64-hanos-ld" "$BUILD/tools/ld"
cd "$BUILD"
export PATH="$BUILD/tools:$CROSS:$PATH"
export CC="$CROSS/x86_64-hanos-gcc" CXX="$CROSS/x86_64-hanos-g++"
export AR="$CROSS/x86_64-hanos-ar" RANLIB="$CROSS/x86_64-hanos-ranlib"
export AR_FOR_TARGET="$CROSS/x86_64-hanos-ar"
export RANLIB_FOR_TARGET="$CROSS/x86_64-hanos-ranlib"
export CC_FOR_BUILD=gcc CXX_FOR_BUILD=g++
export CFLAGS="-O2 -g0 -mcmodel=large -mno-red-zone -fcommon"
export CXXFLAGS="-O2 -g0 -mcmodel=large -mno-red-zone -fcommon"
"$SRC/configure" \
    --build=x86_64-pc-linux-gnu --host=x86_64-hanos --target=x86_64-hanos \
    --prefix=/usr --with-build-sysroot="$SYS" \
    --with-gmp="$SYS/usr" --with-mpfr="$SYS/usr" --with-mpc="$SYS/usr" \
    --disable-nls --disable-shared --enable-static --disable-multilib \
    --enable-languages=c,c++ --disable-bootstrap --disable-libssp \
    --disable-libquadmath --disable-libgomp --disable-libatomic \
    --disable-libvtv --disable-libsanitizer
make -j"$(nproc)" all-gcc all-target-libgcc all-target-libstdc++-v3 MAKEINFO=true
make DESTDIR="$PREFIX" install-gcc install-target-libgcc install-target-libstdc++-v3 MAKEINFO=true

# Strip host binaries. The target archives stay untouched.
find "$PREFIX/usr" -type f ! -name '*.a' | while IFS= read -r f; do
    if file "$f" | grep -q 'ELF'; then
        "$CROSS/x86_64-hanos-strip" --strip-unneeded "$f" 2>/dev/null || true
    fi
done

"$ROOT/scripts/build-libstdcxx-shared.sh"

echo "native gcc: $PREFIX/usr/bin"
