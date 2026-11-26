# Recover the console when the GPU server restarts

## Problem

The GPU owner process exits. The router clears `SVC_GPU`. The restart
callback did not run the server again, so the service stayed unregistered and
the console could not obtain the terminal sprite.

- Trigger: the GPU userspace process exits after it programs the planes.
- Observed: the rain, the cursor and the last sprite content keep scanning. The
  console never draws into the sprite. The terminal area shows a frozen or blank
  window with no status bar.
- Expected: the display server restarts, the router registers the new endpoint,
  and the console draws the terminal into the sprite again.

## Cause

`gpu_server_start()` began with `if (gpu_active) return true;`. `gpu_active`
stays set after the owner exits, so the restart callback skipped endpoint
creation, respawn and `router_register()`. `router_owner_died()` had already
cleared `SVC_GPU`, so the service remained unreachable.

The console resolved the GPU endpoint once at startup and gave up after a fixed
wait. It could not follow a restarted endpoint.

## Fix

- `gpu_server_start()` returns early only while `gpu_active` is set and
  `router_lookup(SVC_GPU)` still returns the endpoint. Otherwise it clears
  `gpu_active`, creates a new endpoint, respawns the server and re-registers.
- The console re-resolves `SVC_GPU` on every attempt, retries the
  `GPU_CONSOLE_SURFACE` request, and falls back to the framebuffer only after
  the retries end.

## Verification

- Broadwell: the terminal and status bar return after the display server
  restarts.
- QEMU: `boot test PASS`, no panic.
- The first exit of the GPU process remains to be traced. This change restores
  the console regardless of the exit cause.

## Files

- `kernel/srv/gpu_srv.c`
- `userspace/servers/console.c`
- `userspace/servers/gpu.c`
