# 2026-10-31: Misc server for clock, entropy and uname

## Changes

- misc: add `include/misc.h` with the `MISC_*` tags and the `utsname` layout.
- misc: add `kernel/srv/misc_srv.{h,c}`. Create the endpoint, grant the HPET window and boot time, spawn `/bin/misc`, register `SVC_MISC`.
- misc: add `userspace/servers/misc.c`. Serve `clock_gettime` from the HPET counter, fill `uname`, and fill `getrandom` from an xorshift seeded by the counter.
- kernel: forward `k_getclock`, `k_uname` and `k_getentropy` to `SVC_MISC`. Keep `clock_now` for the internal absolute-time sleep.
- hpet: expose the physical base, counter period and base counter.
- bootinfo: add the misc HPET window, boot time and version.
- kmain: start the misc server and run `misc_selftest`, which logs `MK: misc PASS`.
- ci: require `MK: misc PASS` in the boot script.
- musltest: print `uname`, `getrandom` and `CLOCK_REALTIME`.
- docs: mark `SVC_MISC` done.

## Verification

- GCC 13.1 and cross GCC 10.2, TCG, `-smp 1`: `MK: selftest PASS`, `MK: misc PASS`, shell starts as `hansh`.
