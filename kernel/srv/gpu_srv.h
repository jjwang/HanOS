/**-----------------------------------------------------------------------------

 @file    gpu_srv.h
 @brief   Spawn the userspace GPU server
 @details
 @verbatim

  Grants the Intel display MMIO and GTT windows to /bin/gpu, registers it as
  SVC_GPU, and forwards pointer deltas to it. The kernel keeps only the boot
  framebuffer for the splash screen.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ipc/ipc.h>
#include <mm/memobj.h>

/* Spawn the GPU server when a supported Intel display device exists.
 * Returns false when the device is absent or the server cannot start. */
bool gpu_server_start(void);

bool gpu_server_active(void);

/* Send a relative pointer delta to the userspace cursor driver. */
void gpu_cursor_move(int32_t dx, int32_t dy);

/* Endpoint the console server uses to request its display surface. */
endpoint_t *gpu_srv_endpoint(void);

/* True when the attached display is Gen9 (Skylake). */
bool gpu_srv_gen9(void);

/* Composite an ARGB canvas into the scanned display surface. */
void gpu_srv_present(memobj_t * mo, int32_t x, int32_t y, int32_t w,
                     int32_t h);
