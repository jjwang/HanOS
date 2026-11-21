# Add uid/gid, login and the first-boot password flow

## Identity

`process_t` carries `uid`, `gid` and `umask`. A new process starts as root with
`umask 022`; a fork copies the parent. `getuid`/`getgid`/`geteuid`/`getegid`
return the values, `setuid`/`setgid` let root switch and let a non-root repeat
its own id, and `umask` stores the create mask. The musl shim forwards these
instead of pinning every process to root.

## Password storage

`libcrypt` is empty in this tree, so the build ships a self-contained SHA-256.
A stored hash is `$5$<salt>$<sha256(salt || password)>`. The shadow file lives on
the ext2 data partition at `/data/etc/shadow`, so a first-boot password survives
a reboot; `build-hdd-image.sh` seeds `root::` and `kite::` with empty hashes.

## Login

`/bin/login` reads `/etc/passwd` for the uid, gid and shell and the shadow for
the hash. An empty hash forces a password, entered twice, and writes it back.
Otherwise the password is checked up to three times. On success login calls
`setgid`/`setuid`, picks an executable shell and execs it. `/bin/passwd` changes
the current user's password. `init` starts `/bin/login` instead of the shell.

## Verification

The login reaches `pw_lookup` in QEMU. Scripted keystroke injection through the
QEMU monitor corrupts the username, so an interactive keyboard test remains.

## Remaining

- `chown`/`fchownat` and `open` mode checks; apply `umask` to file creation.
