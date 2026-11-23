# Keep saving the VFS store under load

## Problem

`/data/etc/shadow` lost the root hash across reboots, so login asked to set the password again. The VFS packs its writable files into `/.vfs-store` on ext2 and saves it a step at a time. The step ran only when the server was idle for the recv timeout; under continuous file traffic (login, bash, exec) the save never finished, so the shadow write stayed as the old store on disk and a reboot dropped it.

## Fix

Advance `store_save_step` on every main-loop pass, not only on idle, so a write is written back within the next few requests.

## Verification

Set the root password, reboot the same image several times: login shows `Password:` instead of asking to set it again.

## Remaining

The save still truncates `/.vfs-store` in place, so an interrupted save leaves it partial. An atomic write (temp file plus rename) needs an ext2 rename op, which the ext2 server does not provide yet.
