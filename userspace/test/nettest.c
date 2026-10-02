/**-----------------------------------------------------------------------------

 @file    nettest.c
 @brief   Loop a datagram through the userspace network server

 @details
 @verbatim

   Creates a datagram socket bound to 127.0.0.1:4444, sends a payload from a
   second socket, and receives it on the first. A mismatch means the loopback
   path or the deferred recvfrom did not work.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>

#include <protocol.h>
#include <stdio.h>
#include <string.h>
#include <sysfunc.h>

/* *INDENT-OFF* */
static command_help_t help_msg[] = {
    {"<help> nettest", "Loop a datagram through the network server."},
};
/* *INDENT-ON* */

#define NET_TEST_PORT   4444

int main(int argc, char *argv[])
{
    (void) argc;
    (void) argv;

    int rx = sys_socket(AF_INET, SOCK_DGRAM, 0);

    if (rx < 0) {
        printf("nettest: socket rx FAIL\n");
        sys_exit(1);
    }
    if (sys_bind(rx, NET_IP_LOOPBACK, NET_TEST_PORT) < 0) {
        printf("nettest: bind FAIL\n");
        sys_exit(1);
    }

    int tx = sys_socket(AF_INET, SOCK_DGRAM, 0);

    if (tx < 0) {
        printf("nettest: socket tx FAIL\n");
        sys_exit(1);
    }

    const char *msg = "hello-net";
    int64_t sent = sys_sendto(tx, NET_IP_LOOPBACK, NET_TEST_PORT, msg,
                              strlen(msg));

    printf("nettest: sent %ld bytes\n", (long) sent);
    if (sent != (int64_t) strlen(msg)) {
        printf("nettest: sendto FAIL\n");
        sys_exit(1);
    }

    char buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t n = sys_recvfrom(rx, buf, sizeof(buf) - 1, &ip, &port);

    if (n <= 0) {
        printf("nettest: recvfrom FAIL %ld\n", (long) n);
        sys_exit(1);
    }
    buf[n] = '\0';

    printf("nettest: recv %ld bytes from %u:%u: %s\n", (long) n, ip, port,
           buf);

    sys_socket_close(rx);
    sys_socket_close(tx);

    if (strcmp(buf, msg) != 0) {
        printf("nettest: payload FAIL\n");
        sys_exit(1);
    }

    printf("nettest: PASS\n");
    sys_exit(0);
}
