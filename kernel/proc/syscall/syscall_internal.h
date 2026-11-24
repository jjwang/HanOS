/**-----------------------------------------------------------------------------

 @file    syscall_internal.h
 @brief   Declarations shared by the syscall modules

 @details
 @verbatim

  Provides the prototypes that core.c needs for handlers implemented in
  signal.c, ipc.c, net.c, time.c and usb.c. Not a public kernel interface.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

#include <fs/vfs.h>
#include <proc/signal.h>

int64_t k_nanosleep(vfs_timespec_t *req, vfs_timespec_t *rem);
int64_t k_clock_nanosleep(int32_t clockid, int32_t flags,
                          vfs_timespec_t *req, vfs_timespec_t *rem);
int32_t k_getclock(void *ignored, int64_t which, vfs_timespec_t *out);
int64_t k_uname(void *buf);
int64_t k_socket(int64_t domain, int64_t type, int64_t protocol);
int64_t k_bind(int64_t sock, int64_t ip, int64_t port);
int64_t k_sendto(int64_t sock, int64_t ip, int64_t port, void *buf,
                 uint64_t len);
int64_t k_recvfrom(int64_t sock, void *buf, uint64_t len, void *ip_ptr,
                   void *port_ptr);
int64_t k_socket_close(int64_t sock);
int64_t k_connect(int64_t sock, int64_t ip, int64_t port);
int64_t k_listen(int64_t sock, int64_t backlog);
int64_t k_accept(int64_t sock);
int64_t k_close(int64_t fd);
int64_t k_kobj_ref(int64_t svc, int64_t id);
int64_t k_kobj_unref(int64_t svc, int64_t id);
int64_t k_kobj_install(int32_t svc, int64_t id);
int64_t k_bind_unix(int64_t sock, void *usaddr, uint64_t len);
int64_t k_connect_unix(int64_t sock, void *usaddr, uint64_t len);
int64_t k_sendmsg(int64_t sock, void *umsg, int64_t flags);
int64_t k_recvmsg(int64_t sock, void *umsg, int64_t flags);
int64_t k_getsockname(int32_t sock, uint32_t *uip, uint16_t *uport);
int64_t k_getpeername(int32_t sock, uint32_t *uip, uint16_t *uport);
int64_t k_shutdown(int32_t sock, int32_t how);
int64_t k_setsockopt(int32_t sock, int32_t level, int32_t optname,
                     const void *optval, uint64_t optlen);
int64_t k_getsockopt(int32_t sock, int32_t level, int32_t optname,
                     void *optval, uint64_t *optlen);
int64_t k_usb_block_info(void);
int64_t k_usb_block_read(int64_t lba, int64_t count, int64_t uptr);
int64_t k_usb_block_write(int64_t lba, int64_t count, int64_t uptr);
int64_t k_sigprocmask(int64_t how, sigset_t *set, sigset_t *oldset);
int64_t k_sigaction(int64_t s, sigaction_t *new, sigaction_t *old);
int64_t k_kill(int32_t pid, int32_t sig);
int64_t k_tkill(int32_t tid, int32_t sig);
int64_t k_tgkill(int32_t tgid, int32_t tid, int32_t sig);
int64_t k_set_tid_address(int32_t *tidptr);
int64_t k_sigreturn(void);
int64_t k_ep_create(void);
int64_t k_ipc_send(int64_t handle, void *umsg);
int64_t k_ipc_recv(int64_t handle, void *umsg);
int64_t k_ipc_call(int64_t handle, void *ureq, void *urep);
int64_t k_ipc_reply(int64_t handle, void *umsg);
int64_t k_handle_close(int64_t handle);
int64_t k_irq_bind(int64_t irq_handle, int64_t ep_handle);
int64_t k_irq_ack(int64_t irq_handle);
int64_t k_ioport_access(int64_t op, int64_t port, int64_t width, int64_t value);
int64_t k_ipc_recv_nb(int64_t handle, void *umsg);
int64_t k_ipc_recv_timeout(int64_t handle, void *umsg, int64_t timeout);
int64_t k_bootinfo(void *ubi);
int64_t k_mem_alloc(int64_t size);
int64_t k_mem_map(int64_t handle, uint64_t vaddr, int64_t prot);
int64_t k_mem_unmap(int64_t handle, uint64_t vaddr);
int64_t k_mem_phys(int64_t handle, uint64_t offset);
int64_t k_handle_dup(int64_t handle);
int64_t k_proc_spawn(int64_t parent_pid, const char *name);
int64_t k_proc_map(int64_t pid, uint64_t vaddr, int64_t memh, int64_t prot);
int64_t k_proc_set_entry(int64_t pid, uint64_t rip, uint64_t rsp);
int64_t k_proc_start(int64_t pid);
