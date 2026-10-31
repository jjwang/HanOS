# 2026-10-31: Build the disk image with make

## Changes

- build: `all` builds `cdrom.iso` and `release/hdd.img`.
- build: both images share one `initrd.tar` target, so a parallel make does not race on the archive.
- docs: update the README build line.

## Verification

- GCC 10.2, TCG: `make` produces `cdrom.iso` and `release/hdd.img`. Both boot to `hansh` with `MK: selftest PASS`, `MK: misc PASS`, `MK: signalfd PASS`.
