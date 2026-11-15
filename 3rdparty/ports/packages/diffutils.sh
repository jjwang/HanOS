#!/bin/bash
# Build GNU diffutils against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=diffutils-3.10
URL=https://ftp.gnu.org/gnu/diffutils/$NAME.tar.xz
fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"
port_configure
make -j"$NPROC"
install_tree diffutils
