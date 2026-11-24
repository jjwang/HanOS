/**-----------------------------------------------------------------------------

 @file    net.c
 @brief   Socket system calls

 @details
 @verbatim

  Forwards socket, bind, sendto, recvfrom, connect, listen, accept and option
  calls to SVC_NET. Copies addresses and option values across the user/kernel
  boundary.

 @endverbatim

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
#include <lib/kmalloc.h>
#include <stdbool.h>
#include <router/router.h>
#include <srv/process_srv.h>
#include <proc/syscall/syscall_internal.h>

/* Linux msghdr and iovec, filled from userspace. */
typedef struct {
    void *iov_base;
    uint64_t iov_len;
} k_iovec_t;

typedef struct {
    void *msg_name;
    uint32_t msg_namelen;
    uint32_t __pad;
    void *msg_iov;
    uint64_t msg_iovlen;
    void *msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
    uint32_t __pad2;
} k_msghdr_t;

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

/* Copy the sun_path of a user sockaddr_un into a kernel buffer. */
static int copy_sockaddr_un(void *usaddr, uint64_t len, char path[108])
{
    uint8_t k[110];

    if (usaddr == NULL || len < 3 || len > sizeof(k))
        return -1;
    if (copy_from_user(k, usaddr, len) != 0)
        return -1;

    uint64_t plen = len - 2;
    uint64_t n = 0;

    if (plen > 107)
        plen = 107;
    while (n < plen && k[2 + n] != 0)
        n++;
    memcpy(path, k + 2, n);
    path[n] = '\0';
    return (int) n;
}

int64_t k_bind_unix(int64_t sock, void *usaddr, uint64_t len)
{
    char path[108];
    int n;

    cpu_set_errno(0);
    n = copy_sockaddr_un(usaddr, len, path);
    if (n < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int64_t r = net_bind_unix((int32_t) sock, path, (uint64_t) n);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EINVAL : (int32_t) - r);
        return -1;
    }
    return 0;
}

int64_t k_connect_unix(int64_t sock, void *usaddr, uint64_t len)
{
    char path[108];
    int n;

    cpu_set_errno(0);
    n = copy_sockaddr_un(usaddr, len, path);
    if (n < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int64_t r = net_connect_unix((int32_t) sock, path, (uint64_t) n);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : (int32_t) - r);
        return -1;
    }
    return 0;
}


/* SCM_RIGHTS over unix sockets. A bundle is the fd list of one sendmsg; a
 * recvmsg delivers the oldest bundle queued for its socket. The queue is keyed
 * by the network server's socket slot, which both ends of a pair share. */
#define SOL_SOCKET_K    1
#define SCM_RIGHTS_K    1
#define CMSG_HDR_K      16
#define CMSG_ALIGN_K(x) (((x) + 7) & ~(uint64_t) 7)
#define ANC_MAX         64
#define ANC_BUNDLES     16
#define ANC_FDS         4

typedef struct {
    int32_t svc;
    int64_t id;
} anc_fd_t;

typedef struct {
    uint8_t n;
    anc_fd_t f[ANC_FDS];
} anc_bundle_t;

typedef struct {
    uint64_t len;
    int32_t level;
    int32_t type;
} k_cmsghdr_t;

static anc_bundle_t anc_q[ANC_MAX][ANC_BUNDLES];
static uint8_t anc_head[ANC_MAX], anc_tail[ANC_MAX], anc_count[ANC_MAX];

static bool anc_is_kobj(int32_t svc)
{
    return svc == SVC_EVENT || svc == SVC_EPOLL || svc == SVC_SIGNALFD
        || svc == SVC_MEMFD;
}

static int anc_push(int32_t target, const anc_bundle_t *b)
{
    if (target <= 0 || target >= ANC_MAX || b->n == 0)
        return -1;
    if (anc_count[target] >= ANC_BUNDLES)
        return -1;
    anc_q[target][anc_tail[target]] = *b;
    anc_tail[target] = (uint8_t) ((anc_tail[target] + 1) % ANC_BUNDLES);
    anc_count[target]++;
    return 0;
}

static int anc_pop(int32_t target, anc_bundle_t *b)
{
    if (target <= 0 || target >= ANC_MAX || anc_count[target] == 0)
        return -1;
    *b = anc_q[target][anc_head[target]];
    anc_head[target] = (uint8_t) ((anc_head[target] + 1) % ANC_BUNDLES);
    anc_count[target]--;
    return 0;
}

static void anc_undo(int32_t target)
{
    if (target <= 0 || target >= ANC_MAX || anc_count[target] == 0)
        return;
    anc_tail[target] = (uint8_t) ((anc_tail[target] + ANC_BUNDLES - 1)
                                  % ANC_BUNDLES);
    anc_count[target]--;
}

/* Parse SCM_RIGHTS fds from a user control buffer and take a kernel reference
 * to each kernel-object descriptor. Returns the bundle count. */
static uint8_t cmsg_collect_fds(const uint8_t * ctrl, uint64_t clen,
                                anc_bundle_t * b)
{
    uint64_t off = 0;

    b->n = 0;
    while (off + CMSG_HDR_K <= clen && b->n < ANC_FDS) {
        const k_cmsghdr_t *c = (const k_cmsghdr_t *) (ctrl + off);
        uint64_t len = c->len;

        if (len < CMSG_HDR_K || off + len > clen)
            break;
        if (c->level == SOL_SOCKET_K && c->type == SCM_RIGHTS_K) {
            uint64_t nd = (len - CMSG_HDR_K) / 4;
            const uint8_t *d = ctrl + off + CMSG_HDR_K;

            for (uint64_t i = 0; i < nd && b->n < ANC_FDS; i++) {
                int32_t fd;
                int32_t kind = 0, svc = 0;
                int64_t sfd = 0;

                memcpy(&fd, d + i * 4, sizeof(fd));
                if (process_fd_get(fd, &kind, &svc, &sfd, NULL, NULL) == 0
                    && anc_is_kobj(svc) && k_kobj_ref(svc, sfd) == 0) {
                    b->f[b->n].svc = svc;
                    b->f[b->n].id = sfd;
                    b->n++;
                }
            }
        }
        off += CMSG_ALIGN_K(len);
    }
    return b->n;
}

static void anc_unref_bundle(const anc_bundle_t * b)
{
    for (uint8_t i = 0; i < b->n; i++)
        k_kobj_unref(b->f[i].svc, b->f[i].id);
}

/* Write an SCM_RIGHTS cmsg. Returns the aligned byte length. */
static uint64_t cmsg_fill(uint8_t * ctrl, const int32_t * fds, uint8_t n)
{
    uint64_t len = CMSG_HDR_K + (uint64_t) n * 4;
    k_cmsghdr_t *c = (k_cmsghdr_t *) ctrl;

    memset(ctrl, 0, CMSG_ALIGN_K(len));
    c->len = len;
    c->level = SOL_SOCKET_K;
    c->type = SCM_RIGHTS_K;
    memcpy(ctrl + CMSG_HDR_K, fds, (uint64_t) n * 4);
    return CMSG_ALIGN_K(len);
}

int64_t k_sendmsg(int64_t sock, void *umsg, int64_t flags)
{
    k_msghdr_t mh;

    (void) flags;
    cpu_set_errno(0);

    if (umsg == NULL || copy_from_user(&mh, umsg, sizeof(mh)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    uint64_t cap = VFS_IO_BUF_SIZE;
    uint8_t *buf = kmalloc(cap);

    if (buf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    uint64_t off = 0;

    for (uint64_t i = 0; i < mh.msg_iovlen && off < cap; i++) {
        k_iovec_t iv;

        if (copy_from_user(&iv, (uint8_t *) mh.msg_iov
                           + i * sizeof(k_iovec_t), sizeof(iv)) != 0) {
            kmfree(buf);
            cpu_set_errno(EFAULT);
            return -1;
        }
        uint64_t n = iv.iov_len;

        if (n > cap - off)
            n = cap - off;
        if (n > 0 && (iv.iov_base == NULL
                      || copy_from_user(buf + off, iv.iov_base, n) != 0)) {
            kmfree(buf);
            cpu_set_errno(EFAULT);
            return -1;
        }
        off += n;
    }

    anc_bundle_t anc;
    int32_t anc_target = 0;

    anc.n = 0;
    if (mh.msg_control != NULL && mh.msg_controllen > 0) {
        uint8_t ctrl[256];
        uint64_t clen = mh.msg_controllen;

        if (clen > sizeof(ctrl))
            clen = sizeof(ctrl);
        if (copy_from_user(ctrl, mh.msg_control, clen) != 0) {
            kmfree(buf);
            cpu_set_errno(EFAULT);
            return -1;
        }
        cmsg_collect_fds(ctrl, clen, &anc);
        if (anc.n > 0) {
            int32_t peer = 0;

            if (net_peer((int32_t) sock, &peer) == 0
                && anc_push(peer, &anc) == 0) {
                anc_target = peer;
            } else {
                anc_unref_bundle(&anc);
                anc.n = 0;
            }
        }
    }

    int64_t r = net_sendto((int32_t) sock, 0, 0, buf, off);

    kmfree(buf);
    if (r < 0) {
        if (anc_target > 0) {
            anc_undo(anc_target);
            anc_unref_bundle(&anc);
        }
        cpu_set_errno(EIO);
        return -1;
    }
    return r;
}

int64_t k_recvmsg(int64_t sock, void *umsg, int64_t flags)
{
    k_msghdr_t mh;

    (void) flags;
    cpu_set_errno(0);

    if (umsg == NULL || copy_from_user(&mh, umsg, sizeof(mh)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    uint64_t cap = 0;

    for (uint64_t i = 0; i < mh.msg_iovlen; i++) {
        k_iovec_t iv;

        if (copy_from_user(&iv, (uint8_t *) mh.msg_iov
                           + i * sizeof(k_iovec_t), sizeof(iv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        cap += iv.iov_len;
        if (cap > VFS_IO_BUF_SIZE)
            cap = VFS_IO_BUF_SIZE;
    }
    if (cap == 0)
        cap = 1;

    uint8_t *buf = kmalloc(cap);

    if (buf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t n = net_recvfrom((int32_t) sock, buf, cap, &ip, &port);

    if (n < 0) {
        int32_t e = (n == -1) ? EIO : (int32_t) - n;

        kmfree(buf);
        cpu_set_errno(e);
        return -1;
    }

    uint64_t off = 0;

    for (uint64_t i = 0; i < mh.msg_iovlen && off < (uint64_t) n; i++) {
        k_iovec_t iv;

        if (copy_from_user(&iv, (uint8_t *) mh.msg_iov
                           + i * sizeof(k_iovec_t), sizeof(iv)) != 0)
            break;
        uint64_t m = iv.iov_len;

        if (m > (uint64_t) n - off)
            m = (uint64_t) n - off;
        if (m > 0 && iv.iov_base != NULL
            && copy_to_user(iv.iov_base, buf + off, m) != 0)
            break;
        off += m;
    }

    mh.msg_flags = 0;

    uint64_t ucontrol = mh.msg_controllen;
    int32_t self = 0;
    anc_bundle_t anc;
    int pop = -1;

    if (n > 0 && net_self((int32_t) sock, &self) == 0)
        pop = anc_pop(self, &anc);

    mh.msg_controllen = 0;
    if (pop == 0) {
        int32_t fds[ANC_FDS];
        uint8_t nf = 0;

        for (uint8_t i = 0; i < anc.n; i++) {
            int64_t nfd = k_kobj_install(anc.f[i].svc, anc.f[i].id);

            if (nfd >= 0)
                fds[nf++] = (int32_t) nfd;
            else
                k_kobj_unref(anc.f[i].svc, anc.f[i].id);
        }

        if (nf > 0) {
            uint8_t cbuf[64];
            uint64_t need = cmsg_fill(cbuf, fds, nf);

            if (mh.msg_control != NULL && ucontrol >= need
                && copy_to_user(mh.msg_control, cbuf, need) == 0) {
                mh.msg_controllen = need;
            } else {
                for (uint8_t i = 0; i < nf; i++)
                    k_close(fds[i]);
            }
        }
    }

    (void) copy_to_user(umsg, &mh, sizeof(mh));

    kmfree(buf);
    return n;
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
