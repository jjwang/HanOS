/**-----------------------------------------------------------------------------

 @file    tty_srv.h
 @brief   Spawn the userspace tty server
 @details
 @verbatim

  Declares the tty server spawn/status helpers and the kernel-side
  read/write/deliver-key clients used to route /dev/tty traffic.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool tty_server_start(void);
bool tty_server_active(void);

/* Relay a decoded key to the tty server. Returns false when it is not active,
 * so the caller can fall back to the in-kernel event bus. */
bool tty_server_deliver_key(uint8_t key);

/* Blocking read/write against the tty server (reads wait for a key). */
int64_t tty_server_read(void *buf, uint64_t len);
int64_t tty_server_write(const void *buf, uint64_t len);

/* Number of keys buffered in the tty server, or -1. */
int64_t tty_server_pending(void);

/* Key a blocking poll waits on; woken when the kernel relays a key. */
void *tty_server_poll_key(void);
