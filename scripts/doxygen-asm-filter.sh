#!/bin/sh
# Emit C-style comments from an assembly file so Doxygen does not parse the
# instructions. Semicolon comments become one-line doc comments.
awk '
/\/\*/ { inblock = 1 }
inblock { print }
/\*\// { inblock = 0 }
/^[[:space:]]*;/ && !inblock {
    sub(/^[[:space:]]*;/, "", $0)
    print "/**" $0 " */"
}
' "$1"
