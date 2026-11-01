#!/bin/bash
# Build grep against the HanOS musl sysroot.
set -e

. "$(dirname "$0")/../common.sh"

NAME=grep-3.11
URL=https://ftp.gnu.org/gnu/grep/$NAME.tar.xz

fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"

port_configure --disable-perl-regexp

make -j"$NPROC"

install_bin src/grep grep
