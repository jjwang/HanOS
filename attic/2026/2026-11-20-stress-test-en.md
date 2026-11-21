# 2026-11-20: Add the standard stress test

- Add `scripts/stress-test.sh` and `scripts/guest-stress.sh`.
- `make stress` rebuilds `release/hdd.img WITH_PORTS=1` and runs the guest test against a private copy.
- The guest runs `ls`, `free`, `cmp`, Python and a C++ build. The host fails on a guest failure or a kernel or loader error.
- Guard the native toolchain copies so the base image builds without the self-host toolchain.
- The VFS, ext2, process, pipe and end tables now grow on demand; the test exposed a stalled VFS open path, fixed.
- Flush the VFS write cache when a dup replaces a descriptor and drop the synchronous store save from the open path; redirections and compiler temp files now work.
- The ports image passes the stress test.
- The runtime store is written from the idle loop, so runtime files persist without stalling the server.
- The store carries the file mode; chmod changes survive a reboot.
- The store buffer stays at 256 KiB; larger runtime files are not persisted. A streaming save is the follow-up.
