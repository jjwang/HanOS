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

int64_t net_socket(int domain, int type, int protocol);
int64_t net_bind(int sock, uint32_t ip, uint16_t port);
int64_t net_connect(int sock, uint32_t ip, uint16_t port);
int64_t net_listen(int sock, int backlog);
int64_t net_accept(int sock);
int64_t net_sendto(int sock, uint32_t ip, uint16_t port, const void *buf,
                   uint64_t len);
int64_t net_recvfrom(int sock, void *buf, uint64_t len, uint32_t *ip,
                     uint16_t *port);
int64_t net_close(int sock);
