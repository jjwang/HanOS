#!/bin/sh
# Standard in-guest stress test. Run it with bash on a ports image:
#   /data/bin/bash /data/bin/stress
# It prints STRESS BEGIN, then STRESS PASS or STRESS FAIL.
set -u

fail=0

check() {
    desc=$1
    shift
    if ! "$@"; then
        echo "STRESS FAIL: $desc"
        fail=1
    fi
}

echo "STRESS BEGIN"

i=0
while [ "$i" -lt 5 ]; do
    check "ls-bin" /data/bin/ls -lR /bin >/dev/null
    check "ls-data" /data/bin/ls -l /data/bin >/dev/null
    i=$((i + 1))
done

check "free" /bin/free
check "cmp" /data/bin/cmp /data/bin/cmp /data/bin/cmp

if [ -x /data/bin/python3 ]; then
    check "python" /data/bin/python3 -S -c \
        "import math; print(math.floor(3.7))" >/dev/null
fi

if [ -x /data/bin/g++ ]; then
    printf 'int main(void){return 0;}\n' > /root/stress.cpp
    check "g++" /data/bin/g++ /root/stress.cpp -o /root/stress_bin
    check "run" /root/stress_bin
fi

if [ "$fail" -eq 0 ]; then
    echo "STRESS PASS"
else
    echo "STRESS FAIL"
fi
