# Linux ports and bash

## Build option

`make ports` builds the optional Linux packages and regenerates `cdrom.iso` and
`release/hdd.img`. A plain `make` leaves them out.

The build downloads each source archive into `3rdparty/build/ports/dl`, builds
it against the HanOS musl sysroot, and stages the binaries in
`3rdparty/build/ports/root/bin`.

## Packages

| Package | Version |
| --- | --- |
| bash | 5.2.37 |
| coreutils | 9.5 |
| grep | 3.11 |
| sed | 4.9 |
| tar | 1.35 |

## Image layout

- `cdrom.iso`: the packages enter `/bin` inside the initrd. The copy skips the
  HanOS server names (`tty`, `input`, `vfs`, ...), so a coreutils `tty` never
  replaces the tty server.
- `release/hdd.img`: the packages enter `/data/bin` next to the games. The
  initrd keeps the base userspace.
- `/root/.bashrc` sets `PATH=/data/bin:/bin:/usr/bin` and `TERM=hanos`.

## Port notes

- `3rdparty/ports/cc` adds the musl include path after the package flags, so a
  package replacement header wins over the musl one.
- The wrapper links with `-fcommon` and `-z max-page-size=0x2000`. `-fcommon`
  keeps the coreutils termcap globals; the page size drops a 2 MB file gap per
  executable.
- bash builds with `--disable-readline`. Readline switches the tty to
  non-canonical mode and the first read returns EOF.
- The process server keeps a server-side open file description until the last
  duplicated fd closes. bash runs `dup2` and then `close` on every redirection.
- `fcntl` handles `F_DUPFD` and `F_DUPFD_CLOEXEC`, and reports the implicit
  standard descriptors as valid. tar checks fd 0..2 before it starts.
- `getppid` returns the recorded parent pid.

## Limits

- Job control is off. The kernel delivers no signals and keeps no process
  groups.
- A pipeline whose writer is coreutils `ls` does not complete. A pipeline
  whose writer is the base `/bin/ls` completes.
