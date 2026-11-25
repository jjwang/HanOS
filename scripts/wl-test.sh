#!/bin/sh
# Boot release/hdd.img, log in, run /data/bin/wltest, and check the log.
# Exit status 0 when the guest prints "wltest: shm PASS" without kernel errors.
set -u

IMG=${1:-release/hdd.img}
LOG=${2:-/tmp/hanos-wl.log}
MON=${3:-/tmp/hanos-wl.mon}
QMP=${4:-/tmp/hanos-wl.qmp}
SER=${5:-/tmp/hanos-wl.ser}
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

rm -f "$LOG" "$MON" "$QMP" "$SER"
WORK=$(mktemp)
cp "$IMG" "$WORK"

qemu-system-x86_64 $ACCEL -cpu "$CPU" -M q35,smm=off -m 2G -smp "$SMP" \
    -no-reboot -rtc base=localtime \
    -drive id=handisk,if=none,format=raw,file="$WORK" \
    -device ide-hd,drive=handisk,bus=ide.0 \
    -display none -serial "unix:$SER,server=on,wait=off" \
    -monitor "unix:$MON,server,nowait" >/dev/null 2>&1 &
pid=$!

# Bridge the guest serial console: append its output to the log and, once the
# login prompt appears, feed the first-boot login over the serial input the
# input server drains. QMP key injection garbles canonical-mode reads; the
# serial console is the supported path.
python3 - "$SER" "$LOG" <<'PY' &
import socket
import sys
import time

sock_path, log_path = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
for _ in range(120):
    try:
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(0.25)
else:
    raise SystemExit("wl: cannot connect to the serial socket")

log = open(log_path, "ab", buffering=0)
seen = False
tail = b""
lines = [b"root\n", b"hanos\n", b"hanos\n",
         b"/data/bin/wltest\n"]

while True:
    try:
        data = s.recv(4096)
    except OSError:
        break
    if not data:
        break
    log.write(data)
    if not seen:
        # The tty mirrors output to the serial line-buffered, so the login
        # prompt (no newline) is not flushed. Trigger on the kernel's login
        # exec line, then feed one line at a time: login reads the user with
        # fgets (stdio buffered) but the passwords with raw read, so a single
        # bulk write is swallowed by the stdio buffer.
        tail = (tail + data)[-128:]
        if b"/bin/login" in tail:
            time.sleep(3.0)
            for line in lines:
                for byte in line:
                    s.sendall(bytes([byte]))
                    time.sleep(0.02)
                time.sleep(1.5)
            seen = True
PY
bridge=$!

inject_keys() {
    i=0
    while [ "$i" -lt 240 ]; do
        grep -aq 'wltest: keymap' "$LOG" 2>/dev/null && break
        sleep 0.5; i=$((i + 1))
    done
    python3 - "$MON" <<'PY2'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
for _ in range(40):
    try:
        s.connect(sys.argv[1]); break
    except OSError:
        time.sleep(0.25)
for k in ("a", "b", "c"):
    s.sendall(("sendkey %s\n" % k).encode()); time.sleep(0.6)
s.close()
PY2
}
inject_keys &

cleanup() {
    kill -TERM "$bridge" 2>/dev/null || true
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
    [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh|login)"|HanOS login' "$LOG" && break
    sleep 1
    i=$((i + 1))
done

if ! { [ -f "$LOG" ] && grep -aqE 'name "(bash|hansh|login)"|HanOS login' "$LOG"; }; then
    echo "wl: shell did not start" >&2
    cleanup
    tail -30 "$LOG" 2>/dev/null >&2
    exit 1
fi

i=0
while [ "$i" -lt "$WAIT_TEST" ]; do
    grep -aq 'wltest: done\|wltest: connect FAIL\|wltest: globals FAIL' "$LOG" 2>/dev/null && break
    sleep 1
    i=$((i + 1))
done

cleanup

status=0
if ! grep -aq 'wltest: shm PASS' "$LOG"; then
    echo "wl: no wltest shm PASS" >&2
    status=1
fi
if ! grep -aq 'wltest: keymap' "$LOG"; then
    echo "wl: no keymap event" >&2
    status=1
fi
if ! grep -aq 'wltest: key ' "$LOG"; then
    echo "wl: no key event" >&2
    status=1
fi
if grep -aq 'wltest: connect FAIL\|wltest: globals FAIL\|wltest: roundtrip FAIL' "$LOG"; then
    echo "wl: guest reported failure" >&2
    status=1
fi
errors=$(grep -aEi 'panic|exception|corrupt|File header|PermissionError|Out of memory|page fault|Segmentation' "$LOG" |     grep -v 'start_pipeline: pgrp pipe')
if [ -n "$errors" ]; then
    echo "wl: kernel or loader error" >&2
    printf '%s\n' "$errors" >&2
    status=1
fi

if [ "$status" -ne 0 ]; then
    tail -40 "$LOG" >&2
    exit 1
fi

echo "wl: PASS"
exit 0
