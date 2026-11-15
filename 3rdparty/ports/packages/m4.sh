#!/bin/bash
# Build GNU m4 against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=m4-1.4.19
URL=https://ftp.gnu.org/gnu/m4/$NAME.tar.xz
fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"
port_configure
make -j"$NPROC"
install_bin src/m4 m4
