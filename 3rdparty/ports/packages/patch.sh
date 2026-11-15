#!/bin/bash
# Build GNU patch against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=patch-2.7.6
URL=https://ftp.gnu.org/gnu/patch/$NAME.tar.xz
fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"
port_configure
make -j"$NPROC"
install_bin src/patch patch
