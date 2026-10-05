# libvterm on HanOS

The console server (`userspace/servers/console.c`) renders through libvterm.
The kernel does not use it.

## Layout

```
3rdparty/libvterm/          vendored libvterm source (not tracked)
userspace/GNUmakefile       builds libvterm and links it into servers/console
```

## Vendor the source

Fetch libvterm 0.3.3 and unpack it into `3rdparty/libvterm`:

```
mkdir -p 3rdparty/libvterm
curl -L https://github.com/neovim/libvterm/archive/refs/tags/v0.3.3.tar.gz \
    | tar xz -C 3rdparty/libvterm --strip-components=1
```

## Build

The userspace build compiles `src/*.c`, generates the encoding tables from the
`.tbl` files, and links the objects into `servers/console` only. No extra step
is needed.

## Use

The console server creates a `VTerm` sized to the framebuffer grid, feeds the
tty byte stream with `vterm_input_write`, and renders the `VTermScreen` cells.
It maps NL to CR-NL on output, because the tty sends a bare LF and a terminal
keeps the column on LF alone.

## Port notes

- The source is compiled with the userspace flags in `userspace/GNUmakefile`
  (`-nostdinc` against musl), so it must not use glibc-only headers.
- `src/encoding/*.inc` are generated from `src/encoding/*.tbl` at build time.
  `src/fullwidth.inc` ships in the tarball.
