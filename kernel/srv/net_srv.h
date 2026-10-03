/**-----------------------------------------------------------------------------

 @file    net_srv.h
 @brief   Spawn the userspace network server and register it with the router
 @details
 @verbatim

  Declares the network server spawn/status helpers and the socket calls the
  kernel forwards to it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool net_server_start(void);
bool net_server_active(void);

int64_t net_socket(int32_t domain, int32_t type, int32_t protocol);
int64_t net_bind(int32_t sock, uint32_t ip, uint16_t port);
int64_t net_connect(int32_t sock, uint32_t ip, uint16_t port);
int64_t net_listen(int32_t sock, int32_t backlog);
int64_t net_accept(int32_t sock);
int64_t net_sendto(int32_t sock, uint32_t ip, uint16_t port, const void *buf,
                   uint64_t len);
int64_t net_recvfrom(int32_t sock, void *buf, uint64_t len, uint32_t *ip,
                     uint16_t *port);
int64_t net_close(int32_t sock);
int64_t net_getsockname(int32_t sock, uint32_t *ip, uint16_t *port);
int64_t net_getpeername(int32_t sock, uint32_t *ip, uint16_t *port);
