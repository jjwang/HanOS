/**-----------------------------------------------------------------------------

 @file    console_srv.h
 @brief   Spawn the userspace console server and forward output to it

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Spawn the console server with the framebuffer and an output endpoint.
 * Returns false when it could not be started. */
bool console_server_start(void);

bool console_server_active(void);

/* Forward a byte buffer to the console server. Returns false when the server
 * is not active, so the caller can fall back to the in-kernel terminal. */
bool console_write_buf(const char *buf, uint64_t len);
