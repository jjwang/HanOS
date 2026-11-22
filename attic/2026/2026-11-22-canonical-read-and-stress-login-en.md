# Return one line per canonical read and log in over serial

## tty

The tty server's canonical read returned `min(len, buffered)` bytes without stopping at the newline, so one large read drained several buffered lines. login reads the user with `fgets` (stdio buffered) and the passwords with raw `read`; the stdio buffer swallowed the extra lines and the password read starved. Cap the read at the first newline on both the direct and the deferred path.

## stress

The stress harness drove login with QMP PS/2 key events, which garbled canonical-mode input. Feed the first-boot login over the QEMU serial console instead: the input server already drains COM1. A bridge copies guest output to the log and writes one login line at a time.

## Verification

`scripts/stress-test.sh` reaches the shell and prints STRESS PASS.
