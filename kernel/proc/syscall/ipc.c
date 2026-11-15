/**-----------------------------------------------------------------------------

 @file    ipc.c
 @brief   IPC, handle, memory-object and process-control system calls

 @details
 @verbatim

  Implements endpoint creation and message transfer, handle duplication and
  closure, IRQ binding, I/O port access, bootinfo export, memory object mapping
  and the process spawn/map/start primitives.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <bootinfo.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <ipc/object.h>
#include <lib/kmalloc.h>
#include <mm/ipc_buf.h>
#include <mm/memobj.h>
#include <mm/uaccess.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/syscall/syscall_internal.h>

int64_t k_ep_create(void)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    endpoint_t *ep = endpoint_create();
    if (ep == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    handle_t h = handle_alloc(&t->handles, endpoint_object(ep),
                              HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV
                              | HANDLE_RIGHT_TRANSFER);
    /* The creation reference is dropped; the handle owns the object now. */
    object_unref(endpoint_object(ep));

    if (h == HANDLE_INVALID) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

static endpoint_t *k_ipc_resolve(int64_t handle, uint32_t rights)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    kernel_object_t *o = handle_get(&t->handles, (handle_t) handle, rights);
    if (o == NULL || o->type != OBJ_ENDPOINT)
        return NULL;

    return (endpoint_t *) o->impl;
}

/* Rights a receiver gets for a handle the sender transfers. The sender's own
 * rights are not intersected yet (there is no per-handle read of them). */
static uint32_t k_transfer_rights(obj_type_t type)
{
    switch (type) {
    case OBJ_MEMORY:
        return HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_MAP;
    case OBJ_ENDPOINT:
        return HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV;
    case OBJ_IRQ:
        return HANDLE_RIGHT_READ;
    default:
        return HANDLE_RIGHT_READ;
    }
}

int64_t k_ipc_send(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    process_t *t = sched_get_current_process();

    if (ep == NULL || t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (m.xfer_count > 2)
        m.xfer_count = 2;

    /* Move each transferred handle out of the sender's table to the queued
     * message. Move semantics: the sender's handle is closed. */
    kernel_object_t *objs[2];
    uint32_t rights[2];
    uint8_t n = 0;

    for (uint8_t i = 0; i < m.xfer_count; i++) {
        kernel_object_t *o =
            handle_get(&t->handles, m.xfer[i], HANDLE_RIGHT_TRANSFER);
        if (o == NULL) {
            for (uint8_t k = 0; k < n; k++)
                object_unref(objs[k]);
            cpu_set_errno(EINVAL);
            return -1;
        }
        object_ref(o);          /* the reference carried by the message */
        handle_close(&t->handles, m.xfer[i]);
        objs[n] = o;
        rights[n] = k_transfer_rights(o->type);
        m.xfer[i] = 0;
        n++;
    }

    if (ipc_send_objs(ep, &m, objs, rights, n) != 0) {
        for (uint8_t k = 0; k < n; k++)
            object_unref(objs[k]);
        cpu_set_errno(EAGAIN);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

/* Common receive path: install any handles the sender moved onto this message
 * into the receiver's table, then return the (rewritten) message to user space.
 * mode 0 = blocking, 1 = timeout, 2 = non-blocking. */
static int64_t k_ipc_recv_common(endpoint_t *ep, void *umsg, int32_t mode,
                                 time_t timeout)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    kernel_object_t *objs[2];
    uint32_t rights[2];
    uint8_t n = 0;
    int32_t r;

    if (mode == 0)
        r = ipc_recv_objs(ep, &m, objs, rights, &n);
    else
        r = ipc_recv_timeout_objs(ep, &m, objs, rights, &n, timeout);

    if (r != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    m.xfer_count = n;
    for (uint8_t i = 0; i < n; i++) {
        handle_t h = handle_alloc(&t->handles, objs[i], rights[i]);
        object_unref(objs[i]);  /* the handle owns the object now */
        m.xfer[i] = (h == HANDLE_INVALID) ? 0 : h;
    }

    if (copy_to_user(umsg, &m, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_ipc_recv(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 0, 0);
}

int64_t k_ipc_call(int64_t handle, void *ureq, void *urep)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t req, rep;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (ipc_call(ep, &req, &rep) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (copy_to_user(urep, &rep, sizeof(rep)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_ipc_reply(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (ipc_send(ep, &m) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_handle_close(int64_t handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL || handle_close(&t->handles, (handle_t) handle) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_irq_bind(int64_t irq_handle, int64_t ep_handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *io =
        handle_get(&t->handles, (handle_t) irq_handle, 0);
    kernel_object_t *ep =
        handle_get(&t->handles, (handle_t) ep_handle, HANDLE_RIGHT_RECV);

    if (io == NULL || io->type != OBJ_IRQ || ep == NULL
        || ep->type != OBJ_ENDPOINT) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    irq_bind((irq_obj_t *) io->impl, (endpoint_t *) ep->impl);
    cpu_set_errno(0);
    return 0;
}

int64_t k_irq_ack(int64_t irq_handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *io =
        handle_get(&t->handles, (handle_t) irq_handle, 0);
    if (io == NULL || io->type != OBJ_IRQ) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    irq_ack((irq_obj_t *) io->impl);
    cpu_set_errno(0);
    return 0;
}

int64_t k_ioport_access(int64_t op, int64_t port, int64_t width,
                        int64_t value)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    bool allowed = false;
    if (t != NULL) {
        for (uint8_t i = 0; i < t->io_port_count; i++) {
            if (port >= t->io_ports[i].first
                && port <= t->io_ports[i].last) {
                allowed = true;
                break;
            }
        }
    }

    if (!allowed) {
        cpu_set_errno(EPERM);
        return -1;
    }

    if (op == 0) {              /* input */
        uint64_t v = 0;
        if (width == 1)
            v = port_inb((uint16_t) port);
        else if (width == 2)
            v = port_inw((uint16_t) port);
        else
            v = port_ind((uint16_t) port);
        return (int64_t) v;
    }

    if (width == 1)
        port_outb((uint16_t) port, (uint8_t) value);
    else if (width == 2)
        port_outw((uint16_t) port, (uint16_t) value);
    else
        port_outd((uint16_t) port, (uint32_t) value);

    return 0;
}

int64_t k_ipc_recv_nb(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 2, 0);
}

int64_t k_ipc_recv_timeout(int64_t handle, void *umsg, int64_t timeout)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 1, (time_t) timeout);
}

int64_t k_bootinfo(void *ubi)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || t->bootinfo == NULL) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    if (copy_to_user(ubi, t->bootinfo, sizeof(bootinfo_t)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_mem_alloc(int64_t size)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || size <= 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    memobj_t *m = memobj_create((uint64_t) size);
    if (m == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    handle_t h = handle_alloc(&t->handles, memobj_object(m),
                              HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                              | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
    object_unref(memobj_object(m));

    if (h == HANDLE_INVALID) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

static memobj_t *k_mem_resolve(int64_t handle, uint32_t rights)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    kernel_object_t *o = handle_get(&t->handles, (handle_t) handle, rights);
    if (o == NULL || o->type != OBJ_MEMORY)
        return NULL;

    return (memobj_t *) o->impl;
}

int64_t k_mem_map(int64_t handle, uint64_t vaddr, int64_t prot)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);

    if (t == NULL || m == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (memobj_map(m, t->addrspace, vaddr, (uint32_t) prot) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_mem_unmap(int64_t handle, uint64_t vaddr)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);
    if (t == NULL || m == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (memobj_unmap(m, t->addrspace, vaddr) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

/* Physical address of a mapped memory object at a byte offset. Used by device
 * servers that program DMA. */
int64_t k_mem_phys(int64_t handle, uint64_t offset)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);

    if (t == NULL || m == NULL || offset >= memobj_size(m)) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) (memobj_page(m, offset / PAGE_SIZE)
                      + (offset & (PAGE_SIZE - 1)));
}

int64_t k_handle_dup(int64_t handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    handle_t h = handle_dup(&t->handles, (handle_t) handle);
    if (h == HANDLE_INVALID) {
        cpu_set_errno(EBADF);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

/* --- Process services primitives (used by the process server) ------------- */

/* Create an empty user process, child of parent_pid. It is not scheduled until
 * PROC_START. */
int64_t k_proc_spawn(int64_t parent_pid, const char *name)
{
    cpu_set_errno(0);

    char kname[64];
    if (name == NULL || strncpy_from_user(kname, name, sizeof(kname)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *parent = process_lookup((pid_t) parent_pid);
    process_t *tc = process_make(kname, NULL, 0, PROC_USER_MODE,
                                 parent == NULL ? NULL : parent->addrspace);
    if (tc == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    if (parent != NULL) {
        tc->ppid = parent->pid;
        process_set_cwd(tc, parent->cwd);
    }

    return (int64_t) tc->pid;
}

/* Copy the pages of memory object memh into process pid at vaddr. The copy
 * keeps the pages owned by the target process, so the server can drop the
 * object right away. */
int64_t k_proc_map(int64_t pid, uint64_t vaddr, int64_t memh, int64_t prot)
{
    cpu_set_errno(0);

    process_t *cur = sched_get_current_process();
    process_t *t = process_lookup((pid_t) pid);

    if (cur == NULL || t == NULL || t->addrspace == NULL
        || (vaddr & (PAGE_SIZE - 1))) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *o = handle_get(&cur->handles, (handle_t) memh,
                                    HANDLE_RIGHT_MAP);
    if (o == NULL || o->type != OBJ_MEMORY) {
        cpu_set_errno(EBADF);
        return -1;
    }

    memobj_t *m = (memobj_t *) o->impl;
    uint64_t np = memobj_page_count(m);
    uint64_t pf = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;
    if (prot & PROT_WRITE)
        pf |= VMM_FLAG_READWRITE;

    for (uint64_t i = 0; i < np; i++) {
        uint64_t dst =
            VIRT_TO_PHYS(kmalloc_chunk(PAGE_SIZE, __func__, __LINE__));
        if (dst == 0) {
            cpu_set_errno(ENOMEM);
            return -1;
        }
        memcpy((void *)PHYS_TO_VIRT(dst),
               (void *)PHYS_TO_VIRT(memobj_page(m, i)), PAGE_SIZE);
        vmm_map(t->addrspace, vaddr + i * PAGE_SIZE, dst, 1, pf);

        vma_add(&t->addrspace->vmas, vaddr + i * PAGE_SIZE,
                vaddr + (i + 1) * PAGE_SIZE, dst, pf);
    }

    return 0;
}

/* Set process pid's initial instruction pointer and stack pointer. */
int64_t k_proc_set_entry(int64_t pid, uint64_t rip, uint64_t rsp)
{
    cpu_set_errno(0);

    process_t *t = process_lookup((pid_t) pid);
    if (t == NULL || t->context == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    process_regs_t *regs = (process_regs_t *) PHYS_TO_VIRT((uint64_t) t->context);
    regs->rip = rip;
    if (rsp != 0)
        regs->rsp = rsp;
    return 0;
}

/* Schedule a process created by PROC_SPAWN. */
int64_t k_proc_start(int64_t pid)
{
    cpu_set_errno(0);

    process_t *t = process_lookup((pid_t) pid);
    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    sched_add(t);
    return 0;
}




