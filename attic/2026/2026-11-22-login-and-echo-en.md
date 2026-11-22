# Run login on the ports image and restore tty echo

## login

The ports image carries `/etc/ports-image`, and init exec'd `/data/bin/bash` directly. login never started, so the first-boot password prompt never appeared. init now waits for the shell and execs `/bin/login` in every case. login selects the shell.

## echo

`pw_read_password` disabled echo, then restored the termios it re-read after disabling it, which left echo off. The shell inherited echo off and printed no input. Restore the termios saved before disabling echo.

## Verification

The ports image prompts for a new root password on first boot. After login the shell echoes typed commands.
