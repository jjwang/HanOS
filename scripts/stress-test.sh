#!/bin/sh
# Boot release/hdd.img and run scripts/guest-stress.sh inside it.
# Exit status 0 when the guest prints STRESS PASS and the log has no kernel or
# loader errors.
set -u

IMG=${1:-release/hdd.img}
LOG=${2:-/tmp/hanos-stress.log}
MON=${3:-/tmp/hanos-stress.mon}
WAIT_BOOT=${WAIT_BOOT:-120}
WAIT_TEST=${WAIT_TEST:-600}

if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL=-enable-kvm
    CPU=host
    SMP=4
else
    ACCEL=
    CPU=qemu64
    SMP=2
fi

rm -f "$LOG" "$MON"
WORK=$(mktemp)
cp "$IMG" "$WORK"

qemu-system-x86_64 $ACCEL -cpu "$CPU" -M q35,smm=off -m 2G -smp "$SMP" \
    -no-reboot -rtc base=localtime \
    -drive id=handisk,if=none,format=raw,file="$WORK" \
    -device ide-hd,drive=handisk,bus=ide.0 \
    -display none -serial "file:$LOG" \
    -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
pid=$!

cleanup() {
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$WORK"
}

i=0
while [ "$i" -lt "$WAIT_BOOT" ]; do
    [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh)"' "$LOG" && break
    sleep 1
    i=$((i + 1))
done

if ! { [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh)"' "$LOG"; }; then
    echo "stress: shell did not start" >&2
    cleanup
    tail -30 "$LOG" 2>/dev/null >&2
    exit 1
fi

sleep 2

python3 - "$MON" <<'PY'
import socket
import sys
import time

s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.5)
keys = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
        '_': 'shift-minus', ',': 'comma', ';': 'semicolon'}
for ch in '/data/bin/bash /data/bin/stress':
    key = keys.get(ch, ch)
    if ch.isupper():
        key = 'shift-' + ch.lower()
    s.sendall(('sendkey %s\n' % key).encode())
    time.sleep(0.16)
time.sleep(0.3)
s.sendall(b'sendkey ret\n')
time.sleep(0.5)
s.close()
PY

i=0
while [ "$i" -lt "$WAIT_TEST" ]; do
    grep -aq 'STRESS PASS\|STRESS FAIL' "$LOG" 2>/dev/null && break
    sleep 1
    i=$((i + 1))
done

cleanup

status=0
if ! grep -aq 'STRESS PASS' "$LOG"; then
    echo "stress: no STRESS PASS" >&2
    status=1
fi
if grep -aq 'STRESS FAIL' "$LOG"; then
    echo "stress: guest reported failure" >&2
    status=1
fi
if grep -aqEi 'panic|exception|corrupt|File header|PermissionError|Out of memory|page fault|Segmentation' "$LOG"; then
    echo "stress: kernel or loader error" >&2
    grep -anEi 'panic|exception|corrupt|File header|PermissionError|Out of memory|page fault|Segmentation' "$LOG" >&2
    status=1
fi

if [ "$status" -ne 0 ]; then
    tail -40 "$LOG" >&2
    exit 1
fi

echo "stress: PASS"
exit 0
