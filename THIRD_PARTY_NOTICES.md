# Third-Party Notices

HanOS is distributed under the MIT License. The following third-party
components are included in the source tree or used at build time.

## Vendored source

- **Limine `limine.h`** — `kernel/3rd-party/boot/limine.h`
  - Copyright (C) 2022-2023 mintsuki and contributors.
  - BSD Zero Clause License (0BSD).
  - The license text is retained at the top of the file.

## Port files

- **musl port files** — `3rdparty/musl/syscall_arch.h`,
  `3rdparty/musl/__set_thread_area.s`, `3rdparty/musl/linker.ld`.
  - musl is Copyright (C) 2005-2020 Rich Felker and contributors.
  - MIT License.
- **libvterm build note** — `3rdparty/libvterm/README.md`.
  - libvterm is Copyright (C) 2008-2023 Paul Evans and contributors.
  - MIT License.

The musl and libvterm sources are downloaded into `3rdparty/build/` at build
time and are not part of this source tree. Their licenses apply to the built
objects and images.
