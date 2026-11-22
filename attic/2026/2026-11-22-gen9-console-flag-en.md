# Pass the Gen9 flag to the console

## Problem

`c3e33e2` gated the console's colour premultiply on `bi.gpu_gen9`. The kernel never filled that field for the console, so the console read zero on every platform and took the opaque path. Gen9 lost the semi-transparent sprite.

## Fix

Add `gpu_srv_gen9()` to report whether the attached display is Gen9, and set `bi.gpu_gen9` from it in `console_spawn_attach`. Gen9 consoles premultiply again. Gen7.5 stays opaque over the kernel framebuffer.

## Verification

Skylake renders the terminal semi-transparent over the rain. Broadwell renders the GPU desktop and terminal normally. Skylake boots three times cleanly.
