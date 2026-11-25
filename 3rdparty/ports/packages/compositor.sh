#!/bin/bash
# Build the Wayland compositor server against the ports stage.
set -e
. "$(dirname "$0")/../common.sh"

SRC="$ROOT/userspace/compositor"
"$PORTS/cc-dynamic" -I"$ROOT/userspace/include" -I"$ROOT/include" \
    -I"$STAGE/include" -L"$STAGE/lib" \
    "$SRC/compositor.c" "$ROOT/userspace/runtime/sysfunc.c" \
    -lwayland-server -lffi -o "$STAGE/bin/compositor"
"$STRIP" --strip-unneeded "$STAGE/bin/compositor"
echo "compositor: $STAGE/bin/compositor"

"$PORTS/cc-dynamic" -I"$STAGE/include" -L"$STAGE/lib" \
    "$SRC/wltest.c" -lwayland-client -lffi -o "$STAGE/bin/wltest"
"$STRIP" --strip-unneeded "$STAGE/bin/wltest"
echo "wltest: $STAGE/bin/wltest"
