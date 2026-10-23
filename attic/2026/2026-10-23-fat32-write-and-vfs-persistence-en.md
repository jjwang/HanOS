# 2026-10-23: FAT32 write and VFS persistence

## Changes

- block: mount FAT partition 0 through `BLOCK_GET_PART`; keep the AHCI SATA path and drop the ATA PIO wording.
- fat32: add `FAT_CREATE`, `FAT_WRITE`, `FAT_TRUNC` and `FAT_UNLINK`; allocate clusters, link chains and update every FAT copy.
- fat32: keep the directory cursor in the server; the transient kernel fd no longer loses it across `readdir`.
- fat32: accept 8.3 names for writes and keep long-name reads.
- vfs: grant the ext2 endpoint to the VFS server and persist runtime files in `/.vfs-store` on partition 1.
- vfs: load the store at boot, save after create, write, truncate, unlink and rename.
- kernel: route `/fat` create, write, truncate and unlink to the FAT server.
- build: create the EFI system partition as FAT32.
- block: wait for the probe thread before init; the disk summary prints before the shell.
