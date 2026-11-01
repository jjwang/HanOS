#!/bin/bash
# Build sed against the HanOS musl sysroot.
set -e

. "$(dirname "$0")/../common.sh"

NAME=sed-4.9
URL=https://ftp.gnu.org/gnu/sed/$NAME.tar.xz

fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"

port_configure

make -j"$NPROC"

install_bin sed/sed sed
