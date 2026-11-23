#!/bin/bash
# Build libwayland against the HanOS musl sysroot.
#
# Two builds run here. First a native wayland-scanner, because the cross build
# needs a host tool of the same version to generate the protocol headers and
# code. Then a meson cross build produces libwayland-client, -server, -cursor
# and -egl plus the guest wayland-scanner, which links expat statically.
set -e
. "$(dirname "$0")/../common.sh"

NAME=wayland-1.22.0
URL=https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.22.0/downloads/wayland-1.22.0.tar.xz

fetch_extract "$NAME" "$URL" wayland-1.22.0.tar.xz
cd "$SRC/$NAME"

# Native scanner for code generation. common.sh exports the cross wrappers, so
# clear them for this build to get the host compiler.
HOSTPREFIX="$BUILD/wayland-host"
rm -rf "$BUILD/wayland-native" "$HOSTPREFIX"
env -u CC -u CXX -u AR -u RANLIB -u STRIP -u CFLAGS -u LDFLAGS \
    meson setup "$BUILD/wayland-native" --prefix="$HOSTPREFIX" \
    -Dlibraries=false -Dscanner=true -Dtests=false -Ddocumentation=false \
    -Ddtd_validation=false
ninja -C "$BUILD/wayland-native" install >/dev/null

HOSTPC=$(dirname "$(find "$HOSTPREFIX" -name wayland-scanner.pc | head -1)")
[ -n "$HOSTPC" ]

cat > "$BUILD/wayland-cross.txt" <<EOF
[binaries]
c = '$PORTS/cc-dynamic'
ar = 'x86_64-elf-ar'
strip = 'x86_64-elf-strip'
ranlib = 'x86_64-elf-ranlib'
pkgconfig = 'pkg-config'

[properties]
pkg_config_libdir = '$STAGE/lib/pkgconfig'

[host_machine]
system = 'linux'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

cat > "$BUILD/wayland-native.txt" <<EOF
[properties]
pkg_config_libdir = '$HOSTPC'
EOF

rm -rf "$BUILD/wayland-build"
env -u CC -u CXX -u AR -u RANLIB -u STRIP -u CFLAGS -u LDFLAGS \
    meson setup "$BUILD/wayland-build" --prefix=/usr \
    --cross-file "$BUILD/wayland-cross.txt" \
    --native-file "$BUILD/wayland-native.txt" \
    -Dlibraries=true -Dscanner=true -Dtests=false -Ddocumentation=false \
    -Ddtd_validation=false
ninja -C "$BUILD/wayland-build"

DEST="$BUILD/install-wayland"
rm -rf "$DEST"
DESTDIR="$DEST" ninja -C "$BUILD/wayland-build" install >/dev/null

mkdir -p "$STAGE/lib" "$STAGE/include" "$STAGE/bin" "$STAGE/share"

# Copy each library as a real file under its versioned and dev soname.
stage_lib() {
    local ver=$1 soname=$2 src

    src=$(find "$DEST" -name "$ver" | head -1)
    [ -n "$src" ]
    "$STRIP" --strip-unneeded "$src"
    cp -f "$src" "$STAGE/lib/$ver"
    cp -f "$src" "$STAGE/lib/$soname"
    cp -f "$src" "$STAGE/lib/${soname%.so.*}.so"
}
stage_lib libwayland-client.so.0.22.0 libwayland-client.so.0
stage_lib libwayland-server.so.0.22.0 libwayland-server.so.0
stage_lib libwayland-cursor.so.0.22.0 libwayland-cursor.so.0
stage_lib libwayland-egl.so.1.22.0 libwayland-egl.so.1

find "$DEST" -path '*/include/*' -name '*.h' -exec cp -f {} "$STAGE/include/" \;
find "$DEST" -name '*.pc' -exec cp -f {} "$STAGE/lib/pkgconfig/" \;

SCANNER=$(find "$DEST" -type f -name wayland-scanner | head -1)
[ -n "$SCANNER" ]
"$STRIP" --strip-unneeded "$SCANNER"
cp -f "$SCANNER" "$STAGE/bin/wayland-scanner"

if [ -d "$DEST/usr/share/wayland" ]; then
    cp -a "$DEST/usr/share/wayland" "$STAGE/share/"
fi
if [ -d "$DEST/usr/share/aclocal" ]; then
    cp -a "$DEST/usr/share/aclocal" "$STAGE/share/"
fi

rm -rf "$DEST"
echo "wayland: $STAGE/lib/libwayland-client.so.0 $STAGE/bin/wayland-scanner"
