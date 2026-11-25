#!/bin/bash
# Fetch, build and stage the optional Linux packages for HanOS.
# Sources, build trees and the staged binaries live under 3rdparty/build/ports.
set -e

. "$(dirname "$0")/common.sh"

packages=${*:-bash coreutils grep sed tar make m4 gawk gzip patch diffutils python libffi expat wayland compositor}

mkdir -p "$STAGE/bin" "$SRC"

# Rebuild a package only when its script or a shared input changed. The stage
# directory is kept so unchanged packages stay in place.
for pkg in $packages; do
    script="$PORTS/packages/$pkg.sh"
    stamp="$BUILD/.pkg-$pkg"

    if [ -f "$stamp" ] && [ "$stamp" -nt "$script" ] \
        && [ "$stamp" -nt "$PORTS/cc" ] && [ "$stamp" -nt "$PORTS/cc-dynamic" ] \
        && [ "$stamp" -nt "$PORTS/common.sh" ]; then
        echo "== $pkg (up to date) =="
        continue
    fi
    echo "== $pkg =="
    "$script"
    touch "$stamp"
done

touch "$BUILD/.installed"
echo "ports: $STAGE/bin"
