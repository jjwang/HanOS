#!/bin/sh
# Boot cdrom.iso under QEMU without KVM, wait for the shell, run pwd, and
# check the serial log for the boot markers.
set -eu

ISO=${1:-cdrom.iso}
LOG=${2:-/tmp/hanos-ci-boot.log}
MON=${3:-/tmp/hanos-ci-monitor}
ACCEL=${HANOS_QEMU_ACCEL:-kvm}
CPU=${HANOS_QEMU_CPU:-host}
SMP=2
ACCEL_ARG=$ACCEL
if [ "$ACCEL" = tcg ]; then
    ACCEL_ARG=tcg,thread=single
    SMP=1
fi

rm -f "$LOG" "$MON"
qemu-system-x86_64 -accel "$ACCEL_ARG" -cpu "$CPU" -M q35,smm=off -m 2G -smp "$SMP" \
    -no-reboot -rtc base=localtime -boot d -cdrom "$ISO" -display none \
    -serial stdio -monitor "unix:$MON,server,nowait" </dev/null >"$LOG" 2>&1 &
qemu_pid=$!

cleanup() {
    kill "$qemu_pid" 2>/dev/null || true
    wait "$qemu_pid" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

i=0
while [ "$i" -lt 30 ] && [ ! -S "$MON" ]; do
    sleep 1
    i=$((i + 1))
done
if [ ! -S "$MON" ]; then
    echo "QEMU monitor did not start" >&2
    exit 1
fi

i=0
while [ "$i" -lt 600 ]; do
    if grep -aq 'name "hansh"' "$LOG" 2>/dev/null; then
        break
    fi
    sleep 1
    i=$((i + 1))
done

if ! grep -aq 'name "hansh"' "$LOG"; then
    echo "shell did not start" >&2
    qemu-system-x86_64 --version >&2 || true
    ps -p "$qemu_pid" -o pid,stat,etime,cmd >&2 || true
    wc -c "$LOG" >&2 || true
    tail -n 120 "$LOG" >&2 || true
    if [ -S "$MON" ]; then
        printf 'info status\ninfo registers\ninfo block\n' \
            | socat - "UNIX-CONNECT:$MON" >&2 || true
    fi
    exit 1
fi
if grep -aEq 'panic|exception|corrupt' "$LOG"; then
    echo "kernel reported a fault" >&2
    grep -aE 'panic|exception|corrupt' "$LOG" >&2
    exit 1
fi
if ! grep -aq 'MK: selftest PASS' "$LOG"; then
    echo "kernel selftest did not pass" >&2
    exit 1
fi

sendkey() {
    printf 'sendkey %s\n' "$1" | socat - "UNIX-CONNECT:$MON" >/dev/null
    sleep 0.2
}

sendkey ret
sleep 2

i=0
while [ "$i" -lt 3 ]; do
    for key in p w d ret; do
        sendkey "$key"
    done
    sleep 2
    if grep -aq 'execute "/bin/pwd"' "$LOG"; then
        break
    fi
    i=$((i + 1))
done

if ! grep -aq 'execute "/bin/pwd"' "$LOG"; then
    echo "shell did not run pwd" >&2
    tail -n 80 "$LOG" >&2 || true
    exit 1
fi

echo "boot test PASS"
