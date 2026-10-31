#!/bin/sh
# Boot cdrom.iso under QEMU and wait for the shell marker.
# Matches Themelios: q35 + El Torito ISO + serial stdio, no interactive monitor.
set -u

ISO=${1:-cdrom.iso}
LOG=${2:-/tmp/hanos-ci-boot.log}
DBG=${3:-/tmp/hanos-ci-qemu-debug.log}

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL=kvm
    CPU=host
    SMP=2
else
    ACCEL=tcg
    CPU=qemu64
    SMP=1
fi

rm -f "$LOG" "$DBG"

echo "ISO: $ISO ($(wc -c <"$ISO") bytes)"
echo "initrd.tar: $(wc -c <initrd.tar 2>/dev/null || echo 0) bytes"
tar -tf initrd.tar | grep -qx bin/hansh || {
    echo "initrd.tar is missing bin/hansh" >&2
    exit 1
}
xorriso -indev "$ISO" -report_el_torito plain 2>/dev/null | grep -q 'El Torito' || {
    echo "the ISO has no El Torito boot record" >&2
    exit 1
}
echo "QEMU: $(qemu-system-x86_64 --version | head -1)"
echo "ACCEL=$ACCEL CPU=$CPU SMP=$SMP"

qemu-system-x86_64 -accel "$ACCEL" -cpu "$CPU" -M q35 -m 2G -smp "$SMP" \
    -no-reboot -boot d -cdrom "$ISO" \
    -display none -serial "file:$LOG" -monitor none \
    -d int,unimp -D "$DBG" >/dev/null 2>&1 &
qemu_pid=$!

i=0
while [ "$i" -lt 180 ]; do
    if grep -aq 'name "hansh"' "$LOG" 2>/dev/null; then
        break
    fi
    if ! kill -0 "$qemu_pid" 2>/dev/null; then
        echo "qemu exited after ${i}s" >&2
        break
    fi
    if [ $((i % 10)) -eq 0 ]; then
        bytes=0
        [ -s "$LOG" ] && bytes=$(wc -c <"$LOG")
        echo "waiting ${i}s, log bytes: $bytes" >&2
    fi
    sleep 1
    i=$((i + 1))
done

kill "$qemu_pid" 2>/dev/null || true
wait "$qemu_pid" 2>/dev/null || true

if ! grep -aq 'name "hansh"' "$LOG"; then
    echo "shell did not start; serial log bytes: $(wc -c <"$LOG" 2>/dev/null || echo 0)" >&2
    cat "$LOG" >&2
    echo "--- qemu debug tail ---" >&2
    tail -n 200 "$DBG" >&2 2>/dev/null || true
    exit 1
fi
grep -aq 'MK: selftest PASS' "$LOG" || {
    echo "kernel selftest did not pass" >&2
    cat "$LOG" >&2
    exit 1
}
grep -aq 'MK: misc PASS' "$LOG" || {
    echo "misc service did not answer" >&2
    cat "$LOG" >&2
    exit 1
}
grep -aq 'MK: signalfd PASS' "$LOG" || {
    echo "signalfd self-test did not pass" >&2
    cat "$LOG" >&2
    exit 1
}
if grep -aEq 'panic|exception|corrupt' "$LOG"; then
    grep -aE 'panic|exception|corrupt' "$LOG" >&2
    exit 1
fi

echo "boot test PASS"
echo "--- serial log ---"
cat "$LOG"
