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
#define GW_IP           0x0a000202U     /* 10.0.2.2, the QEMU gateway */
#define UDP_ECHO_PORT   9999
#define TCP_ECHO_PORT   9998
#define TCP_SRV_PORT    9997

static int tcp_srv_test(void)
{
    int l = sys_socket(AF_INET, SOCK_STREAM, 0);

    if (l < 0) {
        printf("nettest: srv socket FAIL\n");
        return 1;
    }
    if (sys_bind(l, 0, TCP_SRV_PORT) < 0) {
        printf("nettest: srv bind FAIL\n");
        return 1;
    }
    if (sys_listen(l, 1) < 0) {
        printf("nettest: srv listen FAIL\n");
        return 1;
    }
    printf("nettest: srv listening\n");

    int c = sys_accept(l);

    if (c < 0) {
        printf("nettest: srv accept FAIL\n");
        return 1;
    }
    printf("nettest: srv accepted\n");

    char buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t n = sys_recvfrom(c, buf, sizeof(buf) - 1, &ip, &port);

    if (n <= 0) {
        printf("nettest: srv recv FAIL\n");
        return 1;
    }
    buf[n] = '\0';
    printf("nettest: srv recv %ld: %s\n", (long) n, buf);

    int64_t s = sys_sendto(c, 0, 0, buf, n);

    printf("nettest: srv echo %ld\n", (long) s);

    sys_socket_close(c);
    sys_socket_close(l);
    return (s == n) ? 0 : 1;
}


static int udp_nic_test(void)
{
    int s = sys_socket(AF_INET, SOCK_DGRAM, 0);

    if (s < 0) {
        printf("nettest: udp socket FAIL\n");
        return 1;
    }

    const char *m = "udp-hello";
    int64_t n = sys_sendto(s, GW_IP, UDP_ECHO_PORT, m, strlen(m));

    printf("nettest: udp sent %ld\n", (long) n);
    if (n != (int64_t) strlen(m))
        return 1;

    char buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t r = sys_recvfrom(s, buf, sizeof(buf) - 1, &ip, &port);

    if (r <= 0) {
        printf("nettest: udp recv FAIL %ld\n", (long) r);
        return 1;
    }
    buf[r] = '\0';
    printf("nettest: udp recv %ld: %s\n", (long) r, buf);

    sys_socket_close(s);
    return strcmp(buf, m) == 0 ? 0 : 1;
}

static int tcp_nic_test(void)
{
    int s = sys_socket(AF_INET, SOCK_STREAM, 0);

    if (s < 0) {
        printf("nettest: tcp socket FAIL\n");
        return 1;
    }
    if (sys_connect(s, GW_IP, TCP_ECHO_PORT) < 0) {
        printf("nettest: tcp connect FAIL\n");
        return 1;
    }
    printf("nettest: tcp connected\n");

    const char *m = "tcp-hello";
    int64_t n = sys_sendto(s, 0, 0, m, strlen(m));

    printf("nettest: tcp sent %ld\n", (long) n);
    if (n != (int64_t) strlen(m))
        return 1;

    char buf[64];
    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t r = sys_recvfrom(s, buf, sizeof(buf) - 1, &ip, &port);

    if (r <= 0) {
        printf("nettest: tcp recv FAIL %ld\n", (long) r);
        return 1;
    }
    buf[r] = '\0';
    printf("nettest: tcp recv %ld: %s\n", (long) r, buf);

    sys_socket_close(s);
    return strcmp(buf, m) == 0 ? 0 : 1;
}

int main(int argc, char *argv[])
{
    if (argc > 1 && strcmp(argv[1], "udp") == 0) {
        int rc = udp_nic_test();

        printf("nettest: udp %s\n", rc == 0 ? "PASS" : "FAIL");
        sys_exit(rc);
    }

    if (argc > 1 && strcmp(argv[1], "tcp") == 0) {
        int rc = tcp_nic_test();

        printf("nettest: tcp %s\n", rc == 0 ? "PASS" : "FAIL");
        sys_exit(rc);
    }

    if (argc > 1 && strcmp(argv[1], "srv") == 0) {
        int rc = tcp_srv_test();

        printf("nettest: srv %s\n", rc == 0 ? "PASS" : "FAIL");
        sys_exit(rc);
    }

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
