# 2026-10-12: Boot and exec speed

## Symptom

The kernel spends 6.5 s between the block server and the shell. `SCHED:
execute "/bin/hansh"` to `k_execve: inplace` alone takes 2069 ms, while the
earlier servers take 110 ms each.

## Background

The boot path starts each server with `sched_execve`, which calls `elf_load`
and `vfs_load_file`. The linker pads the single load segment to a 2 MB file
offset, so every userspace image is 2.2 MB and a read copies the whole file.

## Root causes

### 1. The VFS server served the initrd

`vfs_load_file` chose `vfs_load_via_server` as soon as the VFS server
registered. The server maps the initrd and returns each file through an IPC
round trip and a memory object. Loading a 2.2 MB image that way took 2069 ms,
and every later exec paid it. Early servers loaded before the VFS server, so
they read the initrd directly and stayed fast.

### 2. The boot probes did redundant work

`kShell` probed fat32, ext2, vfs and process with read and stat smoke tests,
and `block_server_probe` ran a GPT walk, a sector read and a scratch-sector
write/read self-test.

## Implementation

### Direct initrd read (00a29e4)

- `vfs_load_file` tries `initrd_load` first and returns its buffer on a hit.
- The VFS server serves only paths the initrd does not carry, such as the
  ext2 volume.

### Fewer boot probes (9549326)

- Remove the fat32, ext2, vfs and process probes from `kShell`.
- `block_server_probe` issues one `BLOCK_GET_INFO` so the disk model still
  prints.

## Files Changed

| File | Change |
|------|--------|
| `kernel/fs/vfs.c` | read initrd files before the VFS server |
| `kernel/kmain.c` | drop the fat32, ext2, vfs and process probes |
| `kernel/srv/block_srv.c` | reduce `block_server_probe` to one GET_INFO |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- Boot on `-smp 2` and `-smp 4`: no panic, exception or corrupt line.
- `SCHED: execute "/bin/hansh"` to `k_execve: inplace` falls from 2069 ms to
  184 ms; the block-to-shell chain falls from 6.5 s to 1.7 s.
- Interactive `ls` and `pwd` run; `snake` loads from `/data/bin` and exits 0.

## Not yet done

- The userspace linker aligns each load segment to a 2 MB file offset, so the
  images and the initrd carry 2 MB of padding each; the initrd is 60 MB.
  `-z max-page-size=4096` shrinks a 2.2 MB image to 84 KB and the initrd to
  5 MB, but the console server then faults in `vterm_allocator_malloc` on
  `-smp 4`. Find that fault before shrinking the image.
- The per-exec cost that remains is process creation and scheduling, 180 ms
  for `ls`.
