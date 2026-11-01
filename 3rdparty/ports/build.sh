#!/bin/bash
# Fetch, build and stage the optional Linux packages for HanOS.
# Sources, build trees and the staged binaries live under 3rdparty/build/ports.
set -e

. "$(dirname "$0")/common.sh"

packages=${*:-bash coreutils grep sed tar}

rm -f "$BUILD/.installed"
rm -rf "$STAGE"
mkdir -p "$STAGE/bin" "$SRC"

for pkg in $packages; do
    echo "== $pkg =="
    "$PORTS/packages/$pkg.sh"
done

touch "$BUILD/.installed"
echo "ports: $STAGE/bin"
