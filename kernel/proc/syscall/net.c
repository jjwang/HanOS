/**-----------------------------------------------------------------------------

 @file    net.c
 @brief   Socket system calls

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <mm/uaccess.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <protocol.h>
#include <srv/net_srv.h>

/* --- network sockets ----------------------------------------------------- */

int64_t k_socket(int64_t domain, int64_t type, int64_t protocol)
{
    cpu_set_errno(0);

    if (net_server_active() == false) {
        cpu_set_errno(EAFNOSUPPORT);
        return -1;
    }

    int64_t fd = net_socket((int32_t) domain, (int32_t) type, (int32_t) protocol);

    if (fd < 0) {
        cpu_set_errno(EAFNOSUPPORT);
        return -1;
    }
    return fd;
}

int64_t k_bind(int64_t sock, int64_t ip, int64_t port)
{
    cpu_set_errno(0);

    int64_t r = net_bind((int32_t) sock, (uint32_t) ip, (uint16_t) port);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EINVAL : (int32_t) - r);
        return -1;
    }
    return 0;
}

int64_t k_sendto(int64_t sock, int64_t ip, int64_t port, void *buf,
                 uint64_t len)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && len > 0 && !user_range_ok(t, buf, len)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    int64_t n = net_sendto((int32_t) sock, (uint32_t) ip, (uint16_t) port, buf,
                           len);

    if (n < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    return n;
}

int64_t k_recvfrom(int64_t sock, void *buf, uint64_t len, void *ip_ptr,
                   void *port_ptr)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && len > 0 && !user_range_ok(t, buf, len)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t n = net_recvfrom((int32_t) sock, buf, len, &ip, &port);

    if (n < 0) {
        cpu_set_errno((n == -1) ? EIO : (int32_t) - n);
        return -1;
    }

    if (ip_ptr != NULL
        && copy_to_user(ip_ptr, &ip, sizeof(ip)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (port_ptr != NULL
        && copy_to_user(port_ptr, &port, sizeof(port)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return n;
}

int64_t k_socket_close(int64_t sock)
{
    cpu_set_errno(0);

    if (net_close((int32_t) sock) < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }
    return 0;
}

int64_t k_connect(int64_t sock, int64_t ip, int64_t port)
{
    cpu_set_errno(0);

    if (net_connect((int32_t) sock, (uint32_t) ip, (uint16_t) port) < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    return 0;
}

int64_t k_listen(int64_t sock, int64_t backlog)
{
    cpu_set_errno(0);

    if (net_listen((int32_t) sock, (int32_t) backlog) < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    return 0;
}

int64_t k_accept(int64_t sock)
{
    cpu_set_errno(0);

    int64_t fd = net_accept((int32_t) sock);

    if (fd < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }
    return fd;
}

int64_t k_getsockname(int32_t sock, uint32_t * uip, uint16_t * uport)
{
    uint32_t ip = 0;
    uint16_t port = 0;

    cpu_set_errno(0);

    if (net_getsockname(sock, &ip, &port) < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }
    if (uip != NULL && copy_to_user(uip, &ip, sizeof(ip)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (uport != NULL && copy_to_user(uport, &port, sizeof(port)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}

int64_t k_getpeername(int32_t sock, uint32_t * uip, uint16_t * uport)
{
    uint32_t ip = 0;
    uint16_t port = 0;

    cpu_set_errno(0);

    if (net_getpeername(sock, &ip, &port) < 0) {
        cpu_set_errno(ENOTCONN);
        return -1;
    }
    if (uip != NULL && copy_to_user(uip, &ip, sizeof(ip)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (uport != NULL && copy_to_user(uport, &port, sizeof(port)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}

int64_t k_shutdown(int32_t sock, int32_t how)
{
    (void)sock;
    (void)how;
    cpu_set_errno(0);
    return 0;
}

int64_t k_setsockopt(int32_t sock, int32_t level, int32_t optname,
                     const void *optval, uint64_t optlen)
{
    cpu_set_errno(0);

    if (optval == NULL || optlen == 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    uint64_t value = 0;

    if (optname == SO_REUSEADDR) {
        int32_t v = 0;

        if (optlen < sizeof(v) || copy_from_user(&v, optval, sizeof(v)) != 0) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        value = (v != 0);
    } else if (optname == SO_RCVTIMEO) {
        timeval_t tv;

        if (optlen < sizeof(tv)
            || copy_from_user(&tv, optval, sizeof(tv)) != 0) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        value = tv.tv_sec * 1000 + (tv.tv_usec + 999) / 1000;
    } else {
        cpu_set_errno(ENOPROTOOPT);
        return -1;
    }

    int64_t r = net_setsockopt(sock, level, optname, value);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : (int32_t) - r);
        return -1;
    }
    return 0;
}

int64_t k_getsockopt(int32_t sock, int32_t level, int32_t optname,
                     void *optval, uint64_t * optlen)
{
    cpu_set_errno(0);

    if (optval == NULL || optlen == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    uint64_t len = 0;

    if (copy_from_user(&len, optlen, sizeof(len)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    uint64_t value = 0;
    int64_t r = net_getsockopt(sock, level, optname, &value);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : (int32_t) - r);
        return -1;
    }

    if (optname == SO_REUSEADDR) {
        int32_t v = (int32_t) value;

        if (len < sizeof(v)) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        if (copy_to_user(optval, &v, sizeof(v)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        len = sizeof(v);
    } else if (optname == SO_RCVTIMEO) {
        timeval_t tv;

        tv.tv_sec = value / 1000;
        tv.tv_usec = (value % 1000) * 1000;
        if (len < sizeof(tv)) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        if (copy_to_user(optval, &tv, sizeof(tv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        len = sizeof(tv);
    } else {
        cpu_set_errno(ENOPROTOOPT);
        return -1;
    }

    if (copy_to_user(optlen, &len, sizeof(len)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}
