# 2026-11-20: Add the standard stress test

- Add `scripts/stress-test.sh` and `scripts/guest-stress.sh`.
- `make stress` rebuilds `release/hdd.img WITH_PORTS=1` and runs the guest test against a private copy.
- The guest runs `ls`, `free`, `cmp`, Python and a C++ build. The host fails on a guest failure or a kernel or loader error.
- Guard the native toolchain copies so the base image builds without the self-host toolchain.
- The test drives the VFS descriptor table to grow on demand; it exposed a runtime-file write failure on the ports image.
