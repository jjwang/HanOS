# 2026-10-31: Test matrix pass and shell redirection

## Changes

- musl: forward `uname` and `signalfd4`; the shim returned ENOSYS for both.
- proc: close close-on-exec fds after an in-place exec, so `cloexectest` passes.
- shell: enable `<`, `>` and `>>`; open the file and dup it onto the target fd.
- shell: drop the fds 0/1/2 TODO; the kernel special-cases them.
- printf: note that the kernel builds with `-mgeneral-regs-only`, so the formatter stays integer-only.

## Verification

- `-smp 1/2/4`, TCG: `MK: selftest PASS`, `MK: misc PASS`, `MK: signalfd PASS`, `MK: mm PASS`, shell.
- `musltest`: `uname HanOS ...`, `signalfd n=128 signo=10`, `getrandom`, `CLOCK_REALTIME`.
- `faulttest: PASS`, `cloexec: child fd closed ok`, `exitgrouptest PASS status=42`, `mem` prints usage.
- `nettest udp/tcp/srv` against a host echo: PASS.
- Shell suite `pwd`, `ls`, `cd /bin`, `ls | wc`, `cat`, `wc`; redirection on the ext2 partition.
- Killing the mem server logs `svc: restarting the mem server` and re-registers SVC_MM.
