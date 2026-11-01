#!/bin/bash
# Shared paths and helpers for the HanOS third-party package builds.

set -e

PORTS=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$PORTS/../.." && pwd)
BUILD="$ROOT/3rdparty/build/ports"
DL="$BUILD/dl"
SRC="$BUILD/src"
STAGE="$BUILD/root"
NPROC=$(nproc)

export CC="$PORTS/cc"
export CC_FOR_BUILD=gcc
export AR=x86_64-elf-ar
export RANLIB=x86_64-elf-ranlib
export STRIP=x86_64-elf-strip
export MAKEINFO=true
export HELP2MAN=true

fetch() {
    local url=$1 file=$2

    if [ -f "$DL/$file" ]; then
        return 0
    fi
    echo "fetch $file"
    mkdir -p "$DL"
    rm -f "$DL/$file.part"
    curl -fL --retry 5 --retry-delay 2 --retry-connrefused \
        -o "$DL/$file.part" "$url"
    mv "$DL/$file.part" "$DL/$file"
}

fetch_extract() {
    local name=$1 url=$2 file=$3

    mkdir -p "$SRC"
    if [ ! -d "$SRC/$name" ]; then
        fetch "$url" "$file"
        echo "extract $file"
        tar -xf "$DL/$file" -C "$SRC"
    fi
    [ -d "$SRC/$name" ]
}

port_configure() {
    ./configure --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-musl \
        --prefix=/usr --disable-nls --disable-shared --enable-static "$@"
}

install_bin() {
    local src=$1
    local name=${2:-$(basename "$1")}

    mkdir -p "$STAGE/bin"
    cp -f "$src" "$STAGE/bin/$name"
    "$STRIP" --strip-unneeded "$STAGE/bin/$name"
}

install_tree() {
    local pkg=$1
    local dest="$BUILD/install-$pkg"
    local bin

    rm -rf "$dest"
    make install DESTDIR="$dest" prefix=/usr
    mkdir -p "$STAGE/bin"
    for bin in "$dest/usr/bin/"*; do
        cp -fL "$bin" "$STAGE/bin/"
        "$STRIP" --strip-unneeded "$STAGE/bin/$(basename "$bin")"
    done
    rm -rf "$dest"
}
