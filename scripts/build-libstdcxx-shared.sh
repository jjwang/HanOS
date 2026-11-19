#!/bin/bash
# Build a shared libstdc++ for x86_64-hanos from the cross libstdc++ tree.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/3rdparty/build/toolchain"
CROSS="$TOOL/x86_64-hanos/bin"
SYS="$ROOT/3rdparty/build/sysroot"
BUILD="$TOOL/build-gcc/x86_64-hanos/libstdc++-v3"
SRC="$TOOL/src/gcc-10.2.0/libstdc++-v3"
GCCLIB="$TOOL/x86_64-hanos/lib/gcc/x86_64-hanos/10.2.0"
ARCHIVE="$BUILD/src/.libs/libstdc++.a"
OUT="$TOOL/native-gcc/usr/lib"

[ -d "$BUILD" ] || {
    echo "run scripts/build-gcc.sh first" >&2
    exit 1
}
[ -f "$SYS/usr/lib/libgmp.a" ] || "$ROOT/scripts/build-math-libs.sh"

make -C "$BUILD" clean >/dev/null 2>&1 || true
PATH="$CROSS:$PATH" make -C "$BUILD" -j"$(nproc)" \
    CFLAGS='-O2 -g0 -fPIC' CXXFLAGS='-O2 -g0 -fPIC'

# The generated cxx11 ios_failure assembly uses absolute relocations. Rebuild
# it from source so the object can enter a shared library.
"$CROSS/x86_64-hanos-g++" -std=gnu++11 -fPIC -O2 -g0 \
    -fno-implicit-templates \
    -I"$SRC/include" -I"$BUILD/include/x86_64-hanos" \
    -I"$BUILD/include" -I"$SRC/libsupc++" \
    -c "$SRC/src/c++11/cxx11-ios_failure.cc" \
    -o "$BUILD/cxx11-ios_failure.o"
"$CROSS/x86_64-hanos-ar" d "$ARCHIVE" cxx11-ios_failure.o || true
"$CROSS/x86_64-hanos-ar" r "$ARCHIVE" "$BUILD/cxx11-ios_failure.o"
rm -f "$BUILD/cxx11-ios_failure.o"

"$CROSS/x86_64-hanos-ld" -shared -soname libstdc++.so.6 \
    -o "$OUT/libstdc++.so.6" \
    --whole-archive "$ARCHIVE" --no-whole-archive \
    -L"$SYS/usr/lib" -L"$GCCLIB" -lc -lm -lgcc -z notext
cp -f "$OUT/libstdc++.so.6" "$OUT/libstdc++.so"
"$CROSS/x86_64-hanos-strip" --strip-unneeded "$OUT/libstdc++.so.6" || true

echo "shared libstdc++: $OUT/libstdc++.so.6"
