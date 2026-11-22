#!/bin/sh
# Boot release/hdd.img and run scripts/guest-stress.sh inside it.
# Exit status 0 when the guest prints STRESS PASS and the log has no kernel or
# loader errors.
set -u

IMG=${1:-release/hdd.img}
LOG=${2:-/tmp/hanos-stress.log}
MON=${3:-/tmp/hanos-stress.mon}
QMP=${4:-/tmp/hanos-stress.qmp}
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

rm -f "$LOG" "$MON" "$QMP"
WORK=$(mktemp)
cp "$IMG" "$WORK"

qemu-system-x86_64 $ACCEL -cpu "$CPU" -M q35,smm=off -m 2G -smp "$SMP" \
    -no-reboot -rtc base=localtime \
    -drive id=handisk,if=none,format=raw,file="$WORK" \
    -device ide-hd,drive=handisk,bus=ide.0 \
    -display none -serial "file:$LOG" \
    -monitor "unix:$MON,server,nowait" \
    -qmp "unix:$QMP,server,nowait" >/dev/null 2>&1 &
pid=$!

cleanup() {
    kill -TERM "$pid" 2>/dev/null || true
    i=0
    while [ "$i" -lt 50 ] && kill -0 "$pid" 2>/dev/null; do
        sleep 0.1
        i=$((i + 1))
    done
    kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    rm -f "$WORK"
}

i=0
while [ "$i" -lt "$WAIT_BOOT" ]; do
    [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh)"|HanOS login' "$LOG" && break
    sleep 1
    i=$((i + 1))
done

if ! { [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh)"|HanOS login' "$LOG"; }; then
    echo "stress: shell did not start" >&2
    cleanup
    tail -30 "$LOG" 2>/dev/null >&2
    exit 1
fi

sleep 5

# The shell is behind /bin/login now: log in as root, set the first-boot
# password, then run the guest script.
python3 - "$QMP" "root
hanos
hanos
/data/bin/bash /data/bin/stress" <<'PY'
import json
import socket
import sys
import time

path = sys.argv[1]
text = sys.argv[2]

s = socket.socket(socket.AF_UNIX)
for _ in range(40):
    try:
        s.connect(path)
        break
    except OSError:
        time.sleep(0.5)
else:
    raise SystemExit("stress: cannot connect to the QMP socket")

buf = b""


def cmd(obj):
    global buf
    s.sendall((json.dumps(obj) + "\n").encode())
    while b"\n" not in buf:
        data = s.recv(4096)
        if not data:
            raise SystemExit("stress: QMP closed")
        buf += data
    line, buf = buf.split(b"\n", 1)
    return json.loads(line)


cmd({"execute": "qmp_capabilities"})

codes = {' ': 'spc', '/': 'slash', '.': 'dot', '-': 'minus',
         '_': 'shift-minus', ',': 'comma', ';': 'semicolon', '\n': 'ret'}


def press(qcode):
    for down in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"down": down,
             "key": {"type": "qcode", "data": qcode}}}]}})
        time.sleep(0.05)


for ch in text:
    press(codes.get(ch, ch))
    time.sleep(0.12)
press("ret")
time.sleep(0.4)
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
errors=$(grep -aEi 'panic|exception|corrupt|File header|PermissionError|Out of memory|page fault|Segmentation' "$LOG" |     grep -v 'start_pipeline: pgrp pipe')
if [ -n "$errors" ]; then
    echo "stress: kernel or loader error" >&2
    printf '%s\n' "$errors" >&2
    status=1
fi

if [ "$status" -ne 0 ]; then
    tail -40 "$LOG" >&2
    exit 1
fi

echo "stress: PASS"
exit 0
