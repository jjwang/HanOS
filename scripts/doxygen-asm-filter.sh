#!/bin/sh
# Emit C-style comments from an assembly file so Doxygen does not parse the
# instructions. A run of consecutive semicolon comment lines becomes one doc
# block, so multi-line @file/@brief/@details blocks survive the conversion.
awk '
function flush(   i) {
    if (n == 0)
        return
    print "/**"
    for (i = 1; i <= n; i++)
        print buf[i]
    print " **/"
    n = 0
}
{
    if (inblock) {
        print
        if (/\*\//)
            inblock = 0
        next
    }
    if ($0 ~ /\/\*/) {
        flush()
        inblock = 1
        print
        if (/\*\//)
            inblock = 0
        next
    }
    if ($0 ~ /^[[:space:]]*;/) {
        line = $0
        sub(/^[[:space:]]*;/, "", line)
        buf[++n] = line
        next
    }
    flush()
    print
}
END { flush() }
' "$1"
