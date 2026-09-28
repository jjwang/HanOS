/**-----------------------------------------------------------------------------

 @file    console_srv.h
 @brief   Spawn the userspace console server and forward output to it
 @details
 @verbatim

  Declares the console server spawn/status helpers and the endpoint accessor
  used to hand the output endpoint to the tty server.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <ipc/ipc.h>

/* Spawn the console server with the framebuffer and an output endpoint.
 * Returns false when it could not be started. */
bool console_server_start(void);

bool console_server_active(void);

/* The endpoint the console server receives output on, so another server (the
 * tty server) can forward bytes to it. NULL when the console server is off. */
endpoint_t *console_srv_endpoint(void);

/* Forward a byte buffer to the console server. Returns false when the server
 * is not active, so the caller can fall back to the in-kernel terminal. */
bool console_write_buf(const char *buf, uint64_t len);
