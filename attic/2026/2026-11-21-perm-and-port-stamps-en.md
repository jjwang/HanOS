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

## Remaining

- `fchown` (fd-based) is not wired; only the path forms are.
- A second interactive login with the stored password remains to be checked
  with a real keyboard.
