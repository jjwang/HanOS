# Enforce file permissions and stamp ports per package

## Permissions

`chown`/`fchownat` reach the ext2 server through `VFS_CHOWN` and `EXT2_CHOWN`.
The kernel `k_chown` allows root and rejects others with `EPERM`; a non-root
cannot retarget ownership. `vfs_open_path` checks the owner/group/other bits
against the caller's effective read/write request, with root bypassing the
check. A create applies `0666 & ~umask` before the open returns.

## Ports

`3rdparty/ports/build.sh` keeps a stamp per package (`.pkg-<name>`) and rebuilds
a package only when its script, the compiler wrappers or `common.sh` are newer.
The stage directory is preserved, so unchanged packages stay staged. A full run
skips finished packages instead of rebuilding bash, readline and the rest.

## Verification

Two boots of one image: the first sets the password, the second logs in with
it and starts the shell. `fchown` routes through `fchownat`/`AT_EMPTY_PATH`.
