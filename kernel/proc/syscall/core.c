/**-----------------------------------------------------------------------------

 @file    core.c
 @brief   Syscall dispatch and handlers not split into modules

 @details
 @verbatim

  Owns syscall_funcs and syscall_init. Also keeps the filesystem path and
  descriptor handlers, process control, polling, memory helpers and kernel
  object state. Signals, IPC, sockets, time and USB block live in their own
  files.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <string.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/idt.h>
#include <arch/x64/apic.h>
#include <arch/x64/panic.h>
#include <arch/x64/pci.h>
#include <arch/x64/isr_base.h>
#include <lib/klog.h>
#include <printf.h>
#include <lib/vector.h>
#include <lib/kmalloc.h>
#include <mm/mm.h>
#include <mm/uaccess.h>
#include <device/usb/xhci.h>
#include <mm/memobj.h>
#include <mm/ipc_buf.h>
#include <ipc/object.h>
#include <misc.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <srv/tty_srv.h>
#include <srv/fat32_srv.h>
#include <srv/ext2_srv.h>
#include <router/router.h>
#include <protocol.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/wait.h>
#include <proc/syscall.h>
#include <proc/syscall/syscall_internal.h>
#include <proc/signal.h>
#include <fs/vfs.h>
#include <srv/process_srv.h>
#include <srv/net_srv.h>

extern int64_t syscall_handler();
int32_t k_getclock(void *_ignored, int64_t which, vfs_timespec_t * out);
typedef int64_t(*syscall_ptr_t) (void);

/* Kernel-managed descriptors: eventfd and epoll objects. */
#define EFD_SEMAPHORE       1
#define EFD_NONBLOCK        0x800
#define EVENTFD_MAX         16

/** @brief One eventfd object */
typedef struct {
    bool used;
    int32_t refs;
    uint64_t counter;
    bool semaphore;
    void *wait_key;             /* poll wake key */
    int32_t wait_events;        /* poll event bits the waiter asked for */
} eventfd_obj_t;

static eventfd_obj_t eventfds[EVENTFD_MAX];

#define SIGNALFD_MAX        16

/** @brief One signalfd object */
typedef struct {
    bool used;
    int32_t refs;
    uint64_t mask;              /* signals the descriptor reports */
} signalfd_obj_t;

static signalfd_obj_t signal_fds[SIGNALFD_MAX];

/** @brief Linux signalfd_siginfo, 128 bytes */
typedef struct[[gnu::packed]] {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t __pad[128 - 14 * 4 - 5 * 8 - 2 * 2];
} signalfd_siginfo_t;

static int64_t k_signalfd4(int64_t fd, const void *umask, uint64_t sizemask,
                           int32_t flags);
static int64_t signalfd_read(int64_t id, void *buf, uint64_t count);

#define EPOLL_MAX           16
#define EPOLL_FD_MAX        32

/** @brief One watched descriptor in an epoll object */
typedef struct {
    int32_t fd;
    uint32_t events;
    uint64_t data;
} epoll_item_t;

/** @brief One epoll object */
typedef struct {
    bool used;
    int32_t refs;
    int32_t count;
    epoll_item_t items[EPOLL_FD_MAX];
} epoll_obj_t;

static epoll_obj_t epolls[EPOLL_MAX];

#define MEMFD_MAX           64

/** @brief One memfd object: a shareable page set with a size */
typedef struct {
    bool used;
    int32_t refs;
    memobj_t *mo;
    uint64_t size;
} memfd_obj_t;

static memfd_obj_t memfds[MEMFD_MAX];

static memfd_obj_t *memfd_get(int64_t id)
{
    if (id < 1 || id > MEMFD_MAX || !memfds[id - 1].used)
        return NULL;
    return &memfds[id - 1];
}

static uint32_t kobj_lock;

static void kobj_lock_acquire(void)
{
    while (__atomic_exchange_n(&kobj_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void kobj_lock_release(void)
{
    __atomic_store_n(&kobj_lock, 0, __ATOMIC_RELEASE);
}

/* A kernel object (eventfd/epoll/signalfd) is shared by every fd that refers
 * to it. Fork and dup add a reference; close, exec and exit drop one. The slot
 * is freed when the last reference goes away. */
static int32_t *kobj_refs(int32_t svc, int64_t id)
{
    int64_t slot = id - 1;

    if (id < 1)
        return NULL;
    switch (svc) {
    case SVC_EVENT:
        if (slot < EVENTFD_MAX && eventfds[slot].used)
            return &eventfds[slot].refs;
        break;
    case SVC_EPOLL:
        if (slot < EPOLL_MAX && epolls[slot].used)
            return &epolls[slot].refs;
        break;
    case SVC_SIGNALFD:
        if (slot < SIGNALFD_MAX && signal_fds[slot].used)
            return &signal_fds[slot].refs;
        break;
    case SVC_MEMFD:
        if (slot < MEMFD_MAX && memfds[slot].used)
            return &memfds[slot].refs;
        break;
    }
    return NULL;
}

static void kobj_free_slot(int32_t svc, int64_t id)
{
    int64_t slot = id - 1;

    switch (svc) {
    case SVC_EVENT:
        eventfds[slot].used = false;
        break;
    case SVC_EPOLL:
        epolls[slot].used = false;
        break;
    case SVC_SIGNALFD:
        signal_fds[slot].used = false;
        break;
    case SVC_MEMFD:
        if (memfds[slot].mo != NULL) {
            memobj_unref(memfds[slot].mo);
            memfds[slot].mo = NULL;
        }
        memfds[slot].used = false;
        break;
    }
}

int64_t k_kobj_ref(int64_t svc, int64_t id)
{
    kobj_lock_acquire();
    int32_t *refs = kobj_refs((int32_t) svc, id);

    if (refs != NULL)
        (*refs)++;
    kobj_lock_release();
    return refs != NULL ? 0 : -1;
}

int64_t k_kobj_unref(int64_t svc, int64_t id)
{
    kobj_lock_acquire();
    int32_t *refs = kobj_refs((int32_t) svc, id);
    int64_t ret = -1;

    if (refs != NULL) {
        ret = 0;
        if (--(*refs) <= 0)
            kobj_free_slot((int32_t) svc, id);
    }
    kobj_lock_release();
    return ret;
}

static int64_t eventfd_read(int64_t id, void *buf, uint64_t count);
static int64_t eventfd_write(int64_t id, const void *buf, uint64_t count);

static bool debug_info = false;

/* Copy a user path into a fresh kernel buffer sized to the path. Returns NULL
 * on a bad pointer or a path longer than VFS_MAX_PATH_LEN. */
static char *copy_user_path_alloc(const char *upath)
{
    int64_t n;
    char *kpath;

    if (upath == NULL)
        return NULL;

    n = strnlen_user(upath, VFS_MAX_PATH_LEN);
    if (n < 0)
        return NULL;

    kpath = kmalloc((uint64_t) n + 1);
    if (kpath == NULL)
        return NULL;

    if (strncpy_from_user(kpath, upath, (uint64_t) n + 1) < 0) {
        kmfree(kpath);
        return NULL;
    }
    return kpath;
}

#define EXEC_MAX_ARGS   32
#define EXEC_MAX_STRLEN VFS_MAX_PATH_LEN

static void free_exec_argv(char **kargv)
{
    for (int32_t i = 0; kargv != NULL && kargv[i] != NULL; i++)
        kmfree(kargv[i]);
}

/* Copy a NULL-terminated array of user strings into kernel memory. The caller
 * must provide room for EXEC_MAX_ARGS + 1 entries. Returns 0 on success. */
static int32_t copy_exec_argv(const char *uarr[], char **karr)
{
    for (int32_t i = 0; i <= EXEC_MAX_ARGS; i++)
        karr[i] = NULL;

    if (uarr == NULL)
        return 0;

    for (int32_t i = 0; i < EXEC_MAX_ARGS; i++) {
        uint64_t uptr = 0;

        if (copy_from_user(&uptr, &uarr[i], sizeof(uptr)) != 0)
            return -1;
        if (uptr == 0)
            return 0;

        char *s = kmalloc(EXEC_MAX_STRLEN);
        if (s == NULL)
            return -1;
        if (strncpy_from_user(s, (const char *) uptr, EXEC_MAX_STRLEN) < 0) {
            kmfree(s);
            return -1;
        }
        karr[i] = s;
    }

    return 0;
}

int64_t k_debug_log(char *message)
{
    char kmsg[256];

    if (message == NULL
        || strncpy_from_user(kmsg, message, sizeof(kmsg)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    char *s = strchr(kmsg, '\n');

    if (s != NULL && (*(s + 1) == '\0')) {
        *s = '\0';
    }

    klogi("%s\n", kmsg);

    return strlen(kmsg);
}

/* Write a server's bytes to the serial console. Serialised with the kernel log
 * so the two streams do not interleave mid-line. */
int64_t k_serial_write(const char *ubuf, uint64_t len)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (ubuf == NULL)
        return -1;

    if (len > 256)
        len = 256;

    char buf[256];

    if (len > 0) {
        if (!user_range_ok(t, ubuf, len)
            || copy_from_user(buf, ubuf, len) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    klog_write_raw(buf, len);
    return (int64_t) len;
}

int64_t syscall_post(int64_t ret, void *frame)
{
    process_t *t = sched_get_current_process();

    if (t != NULL && frame != NULL) {
        syscall_regs_t *regs = (syscall_regs_t *) frame;

        signal_deliver(t, regs, ret, regs->rcx, regs->r11, false);
    }

    return ret;
}

int64_t k_runcmd(char *cmd)
{
    char kcmd[64];

    if (cmd == NULL || strncpy_from_user(kcmd, cmd, sizeof(kcmd)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (strcmp(kcmd, "lspci") == 0) {
        pci_list();
        return 0;
    } else {
        cpu_set_errno(EINVAL);
        return -1;
    }
}

int64_t k_getentropy(void *buffer, uint64_t length)
{
    cpu_set_errno(0);

    if (length > MISC_MAX_ENTROPY || buffer == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (length == 0)
        return 0;

    if (router_lookup(SVC_MISC) == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    handle_t h;
    memobj_t *mo = memobj_alloc_handle(length, &h);

    if (mo == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = MISC_GETENTROPY;
    req.words[0] = length;
    req.xfer[0] = h;
    req.xfer_count = 1;

    bool ok = router_forward(SVC_MISC, &req, &rep)
        && (int64_t) rep.words[0] >= 0;

    uint8_t kbuf[MISC_MAX_ENTROPY];

    if (ok)
        memobj_copy_out(mo, kbuf, length, 0);

    memobj_unref(mo);

    if (!ok) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    if (copy_to_user(buffer, kbuf, length) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return (int64_t) length;
}

/* Boot check: the misc syscalls answer through SVC_MISC. Runs after the
 * server starts, from the kernel startup process. The result buffers are
 * kernel stack addresses; the uaccess helpers store to them directly. */
bool misc_selftest(void)
{
    vfs_timespec_t ts;
    misc_utsname_t u;
    uint8_t rnd[16];
    bool ok = true;

    /* A zero monotonic clock means the misc server did not get the HPET
     * window; userspace debounce and timeouts depend on it advancing. */
    if (k_getclock(NULL, CLOCK_MONOTONIC, &ts) != 0
        || (ts.tv_sec == 0 && ts.tv_nsec == 0))
        ok = false;
    if (k_uname(&u) != 0 || u.sysname[0] != 'H' || u.sysname[4] != 'S')
        ok = false;
    if (k_getentropy(rnd, sizeof(rnd)) != (int64_t) sizeof(rnd))
        ok = false;

    klogi("MK: misc %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

/*
 * Need to use prot parameter - PROT_READ (0x01), PROT_WRITE (0x02),
 * PROT_EXEC (0x04).
 */
#define MMAP_FILE_CACHE 8
static struct {
    pid_t pid;
    int64_t fd;
    uint8_t *data;
    uint64_t len;
} mmap_file_cache[MMAP_FILE_CACHE];

static bool mmap_file_load(pid_t pid, int64_t fd, uint8_t **data,
                           uint64_t *len)
{
    for (int32_t i = 0; i < MMAP_FILE_CACHE; i++) {
        if (mmap_file_cache[i].data != NULL && mmap_file_cache[i].pid == pid
            && mmap_file_cache[i].fd == fd) {
            *data = mmap_file_cache[i].data;
            *len = mmap_file_cache[i].len;
            return true;
        }
    }

    if (vfs_seek((vfs_fd_t) fd, 0, 0) != 0)
        return false;

    uint64_t cap = 64 * 1024;
    uint64_t used = 0;
    uint8_t *buf = kmalloc(cap);

    if (buf == NULL)
        return false;

    for (;;) {
        if (used == cap) {
            uint8_t *nb = kmalloc(cap * 2);

            if (nb == NULL)
                goto fail;
            memcpy(nb, buf, used);
            kmfree(buf);
            buf = nb;
            cap *= 2;
        }
        int64_t n = vfs_read((vfs_fd_t) fd, cap - used, buf + used);

        if (n <= 0)
            break;
        used += (uint64_t) n;
    }

    for (int32_t i = 0; i < MMAP_FILE_CACHE; i++) {
        if (mmap_file_cache[i].data == NULL) {
            mmap_file_cache[i].pid = pid;
            mmap_file_cache[i].fd = fd;
            mmap_file_cache[i].data = buf;
            mmap_file_cache[i].len = used;
            *data = buf;
            *len = used;
            return true;
        }
    }

fail:
    kmfree(buf);
    return false;
}

/* Drop the cached image for one open file. A closed fd number is reused for a
 * different file, and the cache must not answer with the old contents. */
static void mmap_file_cache_drop(pid_t pid, int64_t fd)
{
    for (int32_t i = 0; i < MMAP_FILE_CACHE; i++) {
        if (mmap_file_cache[i].data != NULL
            && mmap_file_cache[i].pid == pid && mmap_file_cache[i].fd == fd) {
            kmfree(mmap_file_cache[i].data);
            mmap_file_cache[i].data = NULL;
            mmap_file_cache[i].pid = 0;
            mmap_file_cache[i].fd = -1;
        }
    }
}

static void mmap_file_cache_drop_pid(pid_t pid)
{
    for (int32_t i = 0; i < MMAP_FILE_CACHE; i++) {
        if (mmap_file_cache[i].data != NULL && mmap_file_cache[i].pid == pid) {
            kmfree(mmap_file_cache[i].data);
            mmap_file_cache[i].data = NULL;
            mmap_file_cache[i].pid = 0;
            mmap_file_cache[i].fd = -1;
        }
    }
}

uint64_t k_vm_map(uint64_t * hint, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    cpu_set_errno(0);

    process_t *t = sched_get_current_process();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->pid < 1)
            kpanic("SYSCALL: %s meets corrupted pid\n", __func__);
        as = t->addrspace;
    }

    if (length == 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    bool anonymous = (flags & MAP_ANONYMOUS) != 0;
    int32_t fsvc = 0;
    int64_t fsfd = 0;

    if (!anonymous) {
        process_t *owner = sched_get_current_process();
        int32_t kind = 0;

        if (owner == NULL
            || process_fd_get((int32_t) fd, &kind, &fsvc, &fsfd, NULL, NULL) != 0
            || fsvc == SVC_EVENT || fsvc == SVC_EPOLL || fsvc == SVC_SIGNALFD) {
            cpu_set_errno(EBADF);
            goto err_exit;
        }
    }

    if (as == NULL) {
        cpu_set_errno(EINVAL);
        kpanic("k_vm_map: address space manager does not exist\n");
        goto err_exit;
    }

    /* A memfd maps the shared page set itself, not a private copy of a file. */
    if (!anonymous && fsvc == SVC_MEMFD) {
        memfd_obj_t *mf = memfd_get(fsfd);

        if (mf == NULL || mf->mo == NULL) {
            cpu_set_errno(EINVAL);
            goto err_exit;
        }

        uint64_t osize = memobj_size(mf->mo);
        uint64_t mnp = NUM_PAGES(osize);
        uint64_t mptr = (uint64_t) hint;

        if ((flags & MAP_FIXED) && mptr < PAGE_SIZE) {
            cpu_set_errno(EINVAL);
            goto err_exit;
        }
        if (flags & MAP_FIXED) {
            if (mptr != (uint64_t) NULL)
                vma_remove_range(t->addrspace, &t->addrspace->vmas, mptr,
                                 mptr + mnp * PAGE_SIZE);
        } else {
            mptr = as->anon_next;
            as->anon_next += mnp * PAGE_SIZE;
        }

        if (memobj_map(mf->mo, as, mptr, (uint32_t) prot) != 0) {
            cpu_set_errno(EINVAL);
            goto err_exit;
        }
        /* The VMA records the first page. A fork copies a private mapping, not
         * this shared one, so a child that needs sharing maps the fd again. */
        (void) vma_add(&t->addrspace->vmas, mptr, mptr + mnp * PAGE_SIZE,
                       memobj_page(mf->mo, 0), VMM_FLAGS_USERMODE);
        return mptr;
    }

    uint64_t pf = VMM_FLAGS_USERMODE;
    uint64_t ptr = (uint64_t) hint;
    uint64_t np = NUM_PAGES(length);

    /* Keep the zero page unmapped so a NULL dereference still faults. */
    if ((flags & MAP_FIXED) && ptr < PAGE_SIZE) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    /* Drop any previous mapping over the fixed range. */
    if ((flags & MAP_FIXED) && ptr != (uint64_t) NULL)
        vma_remove_range(t->addrspace, &t->addrspace->vmas, ptr,
                         ptr + np * PAGE_SIZE);

    uint64_t phys_ptr =
        VIRT_TO_PHYS(kmalloc_pages(np * PAGE_SIZE, __func__, __LINE__));

    if (!anonymous) {
        uint8_t *dst = (uint8_t *) PHYS_TO_VIRT(phys_ptr);
        uint8_t *file = NULL;
        uint64_t file_len = 0;
        process_t *owner = sched_get_current_process();

        if (owner == NULL
            || !mmap_file_load(owner->pid, (int64_t) fd, &file, &file_len)) {
                kmfree_pages((void *) PHYS_TO_VIRT(phys_ptr), __func__, __LINE__);
            cpu_set_errno(EIO);
            goto err_exit;
        }
        if (offset < file_len) {
            uint64_t n = file_len - offset;

            if (n > length)
                n = length;
            memcpy(dst, file + offset, n);
        }
    }

    if (flags & MAP_FIXED) {
        /* Keep the requested address. */
    } else {
        /* The virtual address must not derive from the physical address:
         * two chunks a multiple of the page-table span apart would share a
         * slot and overwrite each other. Advance a per-space cursor. */
        ptr = as->anon_next;
        as->anon_next += np * PAGE_SIZE;
    }

    vmm_map(as, ptr, phys_ptr, NUM_PAGES(length), pf);

    /* The loader may map a page-aligned FIXED window over a previous
     * reservation. Keep the PTE mapping even when the VMA table rejects the
     * overlap; a later full unmap replaces the range. */
    (void)vma_add(&t->addrspace->vmas, ptr, ptr + np * PAGE_SIZE, phys_ptr,
                  pf);

    if (debug_info) {
        klogi
            ("k_vm_map: pid %ld #%u 0x%016lx(PML4 0x%016lx) map 0x%016lx to 0x%016lx with %ld "
             "pages, prot 0x%016lx, flags 0x%016lx\n", t->pid,
             vma_count(&t->addrspace->vmas), as, as->PML4, phys_ptr, ptr, np,
             prot, flags);
    }

    return ptr;

  err_exit:
    kloge("k_vm_map: pid %ld 0x%016lx(PML4 0x%016lx) returns NULL in malloc()\n",
          t->pid, as, as->PML4);
    return -1;
}

int64_t k_brk(uint64_t addr)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || t->addrspace == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    if (addr == 0) {
        cpu_set_errno(0);
        return (int64_t) t->brk;
    }

    uint64_t cur = t->brk;
    uint64_t old = (cur + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);
    uint64_t new_end = (addr + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);

    if (addr <= cur) {
        cpu_set_errno(0);
        return (int64_t) cur;
    }

    uint64_t np = (new_end - old) / PAGE_SIZE;
    void *buf = kmalloc_pages(np * PAGE_SIZE, __func__, __LINE__);

    if (buf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    memset(buf, 0, np * PAGE_SIZE);
    uint64_t phys = VIRT_TO_PHYS((uint64_t) buf);
    uint64_t pf = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE | VMM_FLAG_READWRITE;

    vmm_map(t->addrspace, old, phys, np, pf);
    vma_add(&t->addrspace->vmas, old, new_end, phys, pf);

    t->brk = addr;
    cpu_set_errno(0);
    return (int64_t) addr;
}

int64_t k_vm_unmap(void *ptr, uint64_t size)
{
    /* Need to implement memory free */
    cpu_set_errno(0);

    process_t *t = sched_get_current_process();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->pid < 1)
            kpanic("SYSCALL: %s meets corrupted pid\n", __func__);
        as = t->addrspace;
    }

    if (size == 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    uint64_t np = NUM_PAGES(size);
    vma_remove_range(as, &as->vmas, (uint64_t) ptr, (uint64_t) ptr + size);

    if (debug_info) {
        klogi("k_vm_unmap: 0x%016lx(PML4 0x%016lx) unmap 0x%016lx with %ld pages\n",
              as, as->PML4, ptr, np);
    }

    return 0;

  err_exit:
    cpu_set_errno(EINVAL);
    return (uint64_t) NULL;
}

int64_t k_mprotect(void *addr, uint64_t len)
{
    (void) addr;
    (void) len;
    cpu_set_errno(0);
    return 0;
}

static int at_resolve(int64_t dirfd64, const char *path, char *out,
                      uint64_t outsz)
{
    char dir[VFS_MAX_PATH_LEN];
    int64_t n;

    int32_t dirfd = (int32_t) dirfd64;

    if (path == NULL)
        return -1;
    if (path[0] == '/') {
        if ((uint64_t) strlen(path) + 1 > outsz)
            return -1;
        strcpy(out, path);
        return 0;
    }

    process_t *t = sched_get_current_process();

    if (dirfd == -100) {
        const char *cwd = (t != NULL && t->cwd != NULL) ? t->cwd : "/";
        uint64_t cl = strlen(cwd);
        uint64_t pl = strlen(path);

        if (cl + pl + 2 > outsz)
            return -1;
        memcpy(out, cwd, cl);
        if (cl == 0 || out[cl - 1] != '/')
            out[cl++] = '/';
        memcpy(out + cl, path, pl + 1);
        return 0;
    }

    if (t != NULL && t->at_dirfd == dirfd && t->at_path[0] != '\0') {
        uint64_t dl = strlen(t->at_path);

        if (dl + 1 + strlen(path) + 1 > outsz)
            return -1;
        memcpy(out, t->at_path, dl);
        if (dl > 0 && out[dl - 1] != '/')
            out[dl++] = '/';
        strcpy(out + dl, path);
        return 0;
    }

    n = vfs_fd_path(dirfd, dir, sizeof(dir));
    if (n <= 0)
        return -1;
    dir[n - 1] = '\0';

    if (t != NULL && n < (int64_t) sizeof(t->at_path)) {
        t->at_dirfd = dirfd;
        memcpy(t->at_path, dir, (uint64_t) n);
    }

    uint64_t dl = strlen(dir);
    bool slash = (dl > 0 && dir[dl - 1] != '/');

    if (dl + (slash ? 1 : 0) + strlen(path) + 1 > outsz)
        return -1;

    memcpy(out, dir, dl);
    if (slash)
        out[dl++] = '/';
    strcpy(out + dl, path);
    return 0;
}

int64_t k_openat(int64_t dirfd, char *path, int64_t flags, int64_t mode);

int64_t k_open(char *path, int64_t flags, int64_t mode)
{
    return k_openat(-100, path, flags, mode);
}

int64_t k_openat(int64_t dirfd, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void) mode;
    (void) dirfd;
    cpu_set_errno(0);

    /* Copy the user path into the kernel before touching it. */
    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    int32_t svc = SVC_FS;
    char abspath[VFS_MAX_PATH_LEN];
    const char *use = kpath;

    if (dirfd != -100 && kpath[0] != '/') {
        if (at_resolve(dirfd, kpath, abspath, sizeof(abspath)) != 0) {
            kmfree(kpath);
            cpu_set_errno(EBADF);
            return -1;
        }
        use = abspath;
        cwd = "/";
    }

    /* The VFS server owns the namespace and resolves cwd+path; it redirects
     * paths under the FAT mount to the FAT server. */
    vfs_fd_t nfd = vfs_open_path(cwd, use, (int32_t) flags, &svc);

    kmfree(kpath);

    if (nfd == VFS_INVALID_FD) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    if (flags & O_CLOEXEC)
        process_fd_fcntl((int32_t) nfd, F_SETFD, FD_CLOEXEC);

    cpu_set_errno(0);
    return nfd;
}

int64_t k_chmod(char *path, int64_t mode)
{
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    int64_t rc = vfs_chmod_path(cwd, kpath, (uint32_t) (mode & 07777u));

    kmfree(kpath);
    if (rc != 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    return 0;
}

int64_t k_chown(char *path, int64_t uid, int64_t gid)
{
    cpu_set_errno(0);

    process_t *t = sched_get_current_process();
    if (t == NULL || t->uid != 0) {
        cpu_set_errno(EPERM);
        return -1;
    }

    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    const char *cwd = t->cwd;
    int64_t rc = vfs_chown_path(cwd, kpath, (uint32_t) uid, (uint32_t) gid);

    kmfree(kpath);
    if (rc != 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    return 0;
}

int64_t k_fchownat(int64_t dirfd, char *path, int64_t uid, int64_t gid,
                   int64_t flags)
{
    if ((flags & 0x1000) && (path == NULL || *path == '\0')) {
        /* AT_EMPTY_PATH: act on the fd's path. Only the ext2 fd carries one. */
        process_t *t = sched_get_current_process();

        if (t == NULL || t->uid != 0) {
            cpu_set_errno(EPERM);
            return -1;
        }
        int32_t kind = 0;
        int32_t svc = 0;
        int64_t sfd = 0;

        if (process_fd_get((int32_t) dirfd, &kind, &svc, &sfd, NULL, NULL) != 0) {
            cpu_set_errno(EBADF);
            return -1;
        }
        char fpath[VFS_MAX_PATH_LEN];

        if (ext2_fd_path(sfd, fpath, sizeof(fpath)) < 0) {
            cpu_set_errno(EBADF);
            return -1;
        }
        return k_chown(fpath, uid, gid);
    }
    return k_chown(path, uid, gid);
}

int64_t k_unlink(char *path)
{
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogi("k_unlink: %s\n", kpath);

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";

    int64_t r = vfs_unlink_path(cwd, kpath);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}


int64_t k_rmdir(char *dir)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(dir);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    int64_t r = vfs_rmdir_path(cwd, kpath);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno((r == -39) ? ENOTEMPTY : ENOENT);
        return -1;
    }
    return 0;
}

int64_t k_mkdirat(int32_t dirfd, const char *path, int32_t mode)
{
    process_t *t = sched_get_current_process();

    (void) mode;
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    char abspath[VFS_MAX_PATH_LEN];
    const char *use = kpath;

    if (dirfd != -100 && kpath[0] != '/') {
        if (at_resolve(dirfd, kpath, abspath, sizeof(abspath)) != 0) {
            kmfree(kpath);
            cpu_set_errno(EBADF);
            return -1;
        }
        use = abspath;
        cwd = "/";
    }

    int64_t r = vfs_mkdir_path(cwd, use);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }
    return 0;
}

int64_t k_mknodat(int32_t dirfd, const char *path, int64_t mode, int64_t dev)
{
    (void) dev;
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    char abspath[VFS_MAX_PATH_LEN];
    const char *use = kpath;

    if (dirfd != -100 && kpath[0] != '/') {
        if (at_resolve(dirfd, kpath, abspath, sizeof(abspath)) != 0) {
            kmfree(kpath);
            cpu_set_errno(EBADF);
            return -1;
        }
        use = abspath;
        cwd = "/";
    }

    int64_t r = vfs_mknod_path(cwd, use, (uint32_t) mode);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }
    return 0;
}

int64_t k_symlinkat(const char *target, int32_t newdirfd, const char *linkpath)
{
    process_t *t = sched_get_current_process();

    (void) newdirfd;
    cpu_set_errno(0);

    char *ktarget = copy_user_path_alloc(target);
    char *klink = copy_user_path_alloc(linkpath);

    if (ktarget == NULL || klink == NULL) {
        if (ktarget != NULL)
            kmfree(ktarget);
        if (klink != NULL)
            kmfree(klink);
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    int64_t r = vfs_symlink_path(cwd, ktarget, klink);

    kmfree(ktarget);
    kmfree(klink);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }
    return 0;
}

/** @brief Filesystem statistics as the kernel returns them to userspace. */
typedef struct {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    uint64_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
} k_statfs_t;

static void statfs_fill(k_statfs_t * out, const vfs_statfs_t * st)
{
    memset(out, 0, sizeof(*out));
    out->f_type = 0xEF53;
    out->f_bsize = st->bsize;
    out->f_frsize = st->bsize;
    out->f_blocks = st->blocks;
    out->f_bfree = st->bfree;
    out->f_bavail = st->bavail;
    out->f_files = st->files;
    out->f_ffree = st->ffree;
    out->f_namelen = 255;
}

int64_t k_statfs(char *path, void *buf)
{
    cpu_set_errno(0);

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    vfs_statfs_t st;

    int64_t r = vfs_statfs_path(cwd, kpath, &st);

    kmfree(kpath);
    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }

    k_statfs_t out;

    statfs_fill(&out, &st);
    if (copy_to_user(buf, &out, sizeof(out)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}

int64_t k_fstatfs(int64_t fd, void *buf)
{
    cpu_set_errno(0);

    char path[VFS_MAX_PATH_LEN];
    vfs_statfs_t st;
    int64_t n = vfs_fd_path((int32_t) fd, path, sizeof(path));

    if (n <= 0) {
        if (vfs_statfs_path("/", "/", &st) < 0) {
            cpu_set_errno(EIO);
            return -1;
        }
    } else if (vfs_statfs_path("/", path, &st) < 0) {
        cpu_set_errno(EIO);
        return -1;
    }

    k_statfs_t out;

    statfs_fill(&out, &st);
    if (copy_to_user(buf, &out, sizeof(out)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}

/* Build an absolute path from a dirfd. Relative paths under AT_FDCWD are
 * joined with the process cwd; other dirfds resolve through vfs_fd_path. */
static int32_t link_abs_path(int64_t dirfd, const char *path, char *out,
                             uint64_t outsz)
{
    if (path[0] == '/') {
        if ((uint64_t) strlen(path) + 1 > outsz)
            return -1;
        strcpy(out, path);
        return 0;
    }

    if (dirfd == -100) {
        process_t *t = sched_get_current_process();
        const char *cwd = (t != NULL) ? t->cwd : "/";
        uint64_t cl = strlen(cwd);
        uint64_t pl = strlen(path);
        bool slash = (cl > 0 && cwd[cl - 1] != '/');

        if (cl + (slash ? 1 : 0) + pl + 1 > outsz)
            return -1;
        memcpy(out, cwd, cl);
        if (slash)
            out[cl++] = '/';
        memcpy(out + cl, path, pl + 1);
        return 0;
    }

    return at_resolve(dirfd, path, out, outsz);
}

int64_t k_linkat(int32_t olddirfd, const char *oldpath, int32_t newdirfd,
                 const char *newpath, int32_t flags)
{
    (void) flags;
    cpu_set_errno(0);

    char *kold = copy_user_path_alloc(oldpath);
    char *knew = copy_user_path_alloc(newpath);

    if (kold == NULL || knew == NULL) {
        if (kold != NULL)
            kmfree(kold);
        if (knew != NULL)
            kmfree(knew);
        cpu_set_errno(EFAULT);
        return -1;
    }

    char oldabs[VFS_MAX_PATH_LEN];
    char newabs[VFS_MAX_PATH_LEN];

    if (link_abs_path(olddirfd, kold, oldabs, sizeof(oldabs)) != 0
        || link_abs_path(newdirfd, knew, newabs, sizeof(newabs)) != 0) {
        kmfree(kold);
        kmfree(knew);
        cpu_set_errno(EBADF);
        return -1;
    }

    kmfree(kold);
    kmfree(knew);

    int64_t r = vfs_link_path(oldabs, newabs);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }
    return 0;
}

int64_t k_renameat(int32_t olddirfd, const char *oldpath, int32_t newdirfd,
                   const char *newpath)
{
    process_t *t = sched_get_current_process();

    (void) olddirfd;
    (void) newdirfd;
    cpu_set_errno(0);

    char *kold = copy_user_path_alloc(oldpath);
    char *knew = copy_user_path_alloc(newpath);

    if (kold == NULL || knew == NULL) {
        if (kold != NULL)
            kmfree(kold);
        if (knew != NULL)
            kmfree(knew);
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    int64_t r = vfs_rename_path(cwd, kold, knew);

    kmfree(kold);
    kmfree(knew);

    if (r < 0) {
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }
    return 0;
}

int64_t k_readlink(int64_t dirfd, const char *path, void *buffer,
                   uint64_t max_size)
{
    process_t *t = sched_get_current_process();

    (void) dirfd;
    cpu_set_errno(0);

    if (buffer == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (max_size == 0)
        return 0;
    if (max_size > VFS_MAX_PATH_LEN)
        max_size = VFS_MAX_PATH_LEN;

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    char *kbuf = kmalloc(max_size);

    if (kbuf == NULL) {
        kmfree(kpath);
        cpu_set_errno(ENOMEM);
        return -1;
    }

    const char *cwd = (t != NULL) ? t->cwd : "/";
    int64_t r = vfs_readlink_path(cwd, kpath, kbuf, max_size);

    kmfree(kpath);

    if (r < 0) {
        kmfree(kbuf);
        cpu_set_errno((r == -1) ? EIO : -r);
        return -1;
    }

    int64_t n = (int64_t) r;

    if (n > (int64_t) max_size)
        n = (int64_t) max_size;

    if (n > 0 && copy_to_user(buffer, kbuf, (uint64_t) n) != 0) {
        kmfree(kbuf);
        cpu_set_errno(EFAULT);
        return -1;
    }

    kmfree(kbuf);
    return n;
}

static int64_t pread_pwrite(int64_t fd, void *buf, uint64_t count,
                            uint64_t offset, bool write)
{
    int64_t old = (int64_t) vfs_tell((vfs_fd_t) fd);
    int64_t ret;

    cpu_set_errno(0);
    if (vfs_seek((vfs_fd_t) fd, offset, 0) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (write)
        ret = vfs_write((vfs_fd_t) fd, count, buf);
    else
        ret = vfs_read((vfs_fd_t) fd, count, buf);
    (void) vfs_seek((vfs_fd_t) fd, (uint64_t) old, 0);
    if (ret < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    return ret;
}

int64_t k_pread64(int64_t fd, void *buf, uint64_t count, uint64_t offset)
{
    return pread_pwrite(fd, buf, count, offset, false);
}

int64_t k_pwrite64(int64_t fd, const void *buf, uint64_t count,
                   uint64_t offset)
{
    return pread_pwrite(fd, (void *) buf, count, offset, true);
}

int64_t k_seek(int64_t fd, int64_t offset, int64_t whence)
{
    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    cpu_set_errno(0);

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) != 0) {
        cpu_set_errno((fd >= 0 && fd < 3) ? ESPIPE : EBADF);
        return -1;
    }

    /* Pipes and terminals have no file offset. */
    if (svc == SVC_PIPE || svc == SVC_TTY) {
        cpu_set_errno(ESPIPE);
        return -1;
    }

    int64_t ret = vfs_seek(fd, offset, whence);

    if (ret < 0)
        cpu_set_errno(EINVAL);

    return ret;
}

int64_t k_close(int64_t fd)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t != NULL && fd == t->at_dirfd)
        t->at_dirfd = -1;

    klogd("k_close: close fd %ld\n", fd);

    if (t != NULL)
        mmap_file_cache_drop(t->pid, fd);

    if (fd >= 0 && fd < 3) {
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;

        /* An implicit tty fd has no process entry. A redirected standard fd
         * owns a server description and must close it like any other fd. */
        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) != 0)
            return 0;
        return vfs_close(fd);
    }

    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) == 0
        && (svc == SVC_EVENT || svc == SVC_EPOLL || svc == SVC_SIGNALFD
            || svc == SVC_MEMFD)) {
        process_fd_close((int32_t) fd, NULL, NULL, NULL);
        k_kobj_unref(svc, sfd);
        return 0;
    }

    return vfs_close(fd);
}

int64_t k_read(int64_t fd, void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogd("k_read: read %ld from fd %ld\n", count, fd);

    if (fd >= 0 && fd < 3) {
        /* Standard input is the tty unless a dup redirected the fd. */
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;
        uint64_t size = 0, seek = 0;

        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) == 0)
            return vfs_read(fd, count, buf);

        if (fd == STDIN)
            return tty_server_read(buf, count);

        cpu_set_errno(EBADF);
        return -1;
    } else if (fd >= VFS_MIN_FD) {
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;

        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) == 0) {
            if (svc == SVC_EVENT)
                return eventfd_read(sfd, buf, count);
            if (svc == SVC_SIGNALFD)
                return signalfd_read(sfd, buf, count);
            if (svc == SVC_EPOLL) {
                cpu_set_errno(EBADF);
                return -1;
            }
            if (svc == SVC_NET) {
                uint32_t ip = 0;
                uint16_t port = 0;
                int64_t n = net_recvfrom((int32_t) fd, buf, count, &ip,
                                         &port);

                if (n < 0) {
                    cpu_set_errno((n == -1) ? EIO : (int32_t) - n);
                    return -1;
                }
                return n;
            }
        }

        int64_t len = vfs_read(fd, count, buf);
        klogd
            ("k_read: try to read %ld bytes from file %ld and return %ld bytes\n",
             count, fd, len);
        return len;
    } else {
        cpu_set_errno(EBADF);
        return -1;
    }
}

int64_t k_write(int64_t fd, const void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (fd >= 0 && fd < 3) {
        /* Standard output is the tty unless a dup redirected the fd. */
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;
        uint64_t size = 0, seek = 0;

        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) == 0)
            return vfs_write(fd, count, buf);

        if (fd == STDOUT || fd == STDERR)
            return tty_server_write(buf, count);

        cpu_set_errno(EBADF);
        return -1;
    }

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) == 0) {
        if (svc == SVC_EVENT)
            return eventfd_write(sfd, buf, count);
        if (svc == SVC_SIGNALFD) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        if (svc == SVC_EPOLL) {
            cpu_set_errno(EBADF);
            return -1;
        }
        if (svc == SVC_NET) {
            int64_t n = net_sendto((int32_t) fd, 0, 0, buf, count);

            if (n < 0) {
                cpu_set_errno(EIO);
                return -1;
            }
            return n;
        }
    }

    int64_t r = vfs_write(fd, count, buf);

    if (r == -32) {             /* -EPIPE */
        process_t *curr = sched_get_current_process();

        if (curr != NULL)
            signal_raise(curr, SIGPIPE);
        cpu_set_errno(EPIPE);
        return -1;
    }
    if (r < 0) {
        cpu_set_errno((int32_t) -r);
        return -1;
    }
    return r;
}

int64_t k_set_fs_base(uint64_t val)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);
    klogd("k_set_fs_base: process #%ld set to 0x%016lx\n",
          t == NULL ? 0 : t->pid, val);
    write_msr(MSR_FS_BASE, val);
    if (t != NULL)
        t->fs_base = val;
    return 0;
}

static uint16_t tty_win_rows = 25;
static uint16_t tty_win_cols = 80;
static pid_t tty_pgrp;

/* Bash duplicates stderr to a high descriptor. Accept every descriptor backed
 * by the tty server, not only fds 0..2. */
static bool tty_fd(int64_t fd)
{
    process_t *t;
    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (fd >= 0 && fd <= 2)
        return true;
    t = sched_get_current_process();
    if (t == NULL)
        return false;
    return process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) == 0
        && svc == SVC_TTY;
}

int64_t k_ioctl(int64_t fd, int64_t request, int64_t arg)
{
#define TIOCGWINSZ_K 0x5413
#define TIOCSWINSZ_K 0x5414
#define TIOCGPGRP_K 0x540F
#define TIOCSPGRP_K 0x5410
#define TCGETS_K    0x5401
#define TCSETS_K    0x5402
#define TCSETSW_K   0x5403
#define TCSETSF_K   0x5404
#define ICANON_K    0x0002
#define ECHO_K      0x0008

    /* termios, matching musl's layout on x86-64. */
    typedef struct {
        uint32_t c_iflag;
        uint32_t c_oflag;
        uint32_t c_cflag;
        uint32_t c_lflag;
        uint8_t c_line;
        uint8_t c_cc[32];
        uint32_t c_ispeed;
        uint32_t c_ospeed;
    } k_termios_t;

    static uint32_t tty_lflag = 0x0002 | 0x0008;        /* ICANON | ECHO */

    cpu_set_errno(0);

    /* Report a terminal size for the standard descriptors, so isatty() and
     * programs that query the window size work. */
    if (request == TIOCGWINSZ_K) {
        struct {
            uint16_t row;
            uint16_t col;
            uint16_t xpixel;
            uint16_t ypixel;
        } ws = { tty_win_rows, tty_win_cols, 0, 0 };

        if (!tty_fd(fd) || arg == 0
            || copy_to_user((void *) arg, &ws, sizeof(ws)) != 0) {
            cpu_set_errno(ENOTTY);
            return -1;
        }
        return 0;
    }

    /* The console server owns the grid geometry and publishes it here. */
    if (request == TIOCSWINSZ_K) {
        struct {
            uint16_t row;
            uint16_t col;
            uint16_t xpixel;
            uint16_t ypixel;
        } ws;

        if (arg == 0 || copy_from_user(&ws, (void *) arg, sizeof(ws)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        if (ws.row != 0 && ws.col != 0) {
            tty_win_rows = ws.row;
            tty_win_cols = ws.col;
        }
        return 0;
    }

    if (request == TIOCGPGRP_K) {
        pid_t pgrp = tty_pgrp;

        if (pgrp == 0) {
            process_t *t = sched_get_current_process();

            pgrp = (t != NULL) ? t->pid : 0;
        }
        if (!tty_fd(fd) || arg == 0
            || copy_to_user((void *) arg, &pgrp, sizeof(pgrp)) != 0) {
            cpu_set_errno(ENOTTY);
            return -1;
        }
        return 0;
    }

    if (request == TIOCSPGRP_K) {
        pid_t pgrp;

        if (!tty_fd(fd) || arg == 0
            || copy_from_user(&pgrp, (void *) arg, sizeof(pgrp)) != 0) {
            cpu_set_errno(ENOTTY);
            return -1;
        }
        tty_pgrp = pgrp;
        return 0;
    }

    /* termios for the tty: a full-screen program clears ECHO and ICANON. */
    if (request == TCGETS_K || request == TCSETS_K || request == TCSETSW_K
        || request == TCSETSF_K) {
        k_termios_t t;

        if (!tty_fd(fd) || arg == 0) {
            cpu_set_errno(ENOTTY);
            return -1;
        }

        memset(&t, 0, sizeof(t));
        t.c_iflag = 0x0500;             /* ICRNL | IXON */
        t.c_oflag = 0x0005;
        t.c_cflag = 0x00bf;

        if (request == TCGETS_K) {
            t.c_lflag = tty_lflag;
            if (copy_to_user((void *) arg, &t, sizeof(t)) != 0) {
                cpu_set_errno(EFAULT);
                return -1;
            }
            return 0;
        }

        if (copy_from_user(&t, (void *) arg, sizeof(t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        tty_lflag = t.c_lflag;
        tty_server_setmode((t.c_lflag & ECHO_K) != 0,
                           (t.c_lflag & ICANON_K) != 0);
        return 0;
    }

    cpu_set_errno(ENOTTY);
    return -1;
}

int64_t k_fstatat(int64_t dirfd, const char *path, int64_t statbuf,
                  int64_t flags)
{
    (void) flags;

    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    char abspath[VFS_MAX_PATH_LEN];
    const char *use = kpath;
    vfs_stat_t st;

    if (dirfd != -100 && kpath[0] != '/') {
        if (at_resolve(dirfd, kpath, abspath, sizeof(abspath)) != 0) {
            kmfree(kpath);
            cpu_set_errno(EBADF);
            return -1;
        }
        use = abspath;
        cwd = "/";
    }

    int64_t r = vfs_stat_path(cwd, use, &st);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    if (copy_to_user((void *) statbuf, &st, sizeof(st)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}

int64_t k_fstat(int64_t fd, int64_t statbuf)
{
    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        /*
         * Set the file stat buffer to zero. If we do nothing here, maybe it
         * will cause crash in some apps, e.g., cat in coreutils.
         */
        if (clear_user((void *) statbuf, sizeof(vfs_stat_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        klogd("k_fstat: success with fd %ld\n", fd);
        return 0;
    }

    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    cpu_set_errno(0);

    if (desc != NULL && desc->server) {
        vfs_stat_t st;

        memset(&st, 0, sizeof(st));

        if (desc->svc == SVC_FAT) {
            uint64_t size = 0;
            bool is_dir = false;

            if (fat32_fstat_fd(desc->server_fd, &size, &is_dir) < 0) {
                cpu_set_errno(ENOENT);
                return -1;
            }
            st.st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            st.st_nlink = 1;
            st.st_size = size;
        } else if (desc->svc == SVC_EXT) {
            ext2_meta_t m;

            if (ext2_fstat_fd(desc->server_fd, &m) < 0) {
                cpu_set_errno(ENOENT);
                return -1;
            }
            st.st_dev = 2;
            st.st_ino = (ino_t) m.ino;
            st.st_mode = (uint16_t) m.mode;
            st.st_nlink = m.nlink;
            st.st_uid = m.uid;
            st.st_gid = m.gid;
            st.st_size = (off_t) m.size;
            st.st_blksize = 512;
            st.st_blocks = (blkcnt_t) m.blocks;
            st.st_mtim.tv_sec = m.mtime;
        } else if (desc->svc == SVC_NET) {
            st.st_mode = S_IFSOCK | 0600;
            st.st_nlink = 1;
        } else if (vfs_server_fstat(desc->server_fd, &st) < 0) {
            cpu_set_errno(ENOENT);
            return -1;
        }

        if (copy_to_user((void *) statbuf, &st, sizeof(st)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        return 0;
    }

    kloge("k_fstat: fail with fd %ld\n", fd);
    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_faccessat(int64_t dirfd, const char *path, uint64_t mode,
                    uint64_t flags)
{
    cpu_set_errno(0);

    /* The kernel has one uid, so AT_EACCESS is the same as the default check.
     * No other flag is meaningful for access(). */
    if (flags & ~(uint64_t) AT_EACCESS) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    char abspath[VFS_MAX_PATH_LEN];
    const char *use = kpath;

    if (dirfd != -100 && kpath[0] != '/') {
        if (at_resolve(dirfd, kpath, abspath, sizeof(abspath)) != 0) {
            kmfree(kpath);
            cpu_set_errno(EBADF);
            return -1;
        }
        use = abspath;
        cwd = "/";
    }

    klogd("k_faccessat: access \"%s\" at mode 0x%016lx\n", use, mode);

    int64_t r = vfs_access_path(cwd, use, mode);

    kmfree(kpath);

    if (r < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_access(const char *path, uint64_t mode)
{
    return k_faccessat(-100, path, mode, 0);
}

int64_t k_getpid()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t != NULL) {
        klogd("k_getpid: process #%ld\n", t->pid);
        if (t->pid >= 1)
            return t->pid;
    }

    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_getuid(void)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);
    return (t != NULL) ? (int64_t) t->uid : 0;
}

int64_t k_getgid(void)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);
    return (t != NULL) ? (int64_t) t->gid : 0;
}

int64_t k_geteuid(void)
{
    return k_getuid();
}

int64_t k_getegid(void)
{
    return k_getgid();
}

int64_t k_setuid(int64_t uid)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (t->uid != 0 && (uint32_t) uid != t->uid) {
        cpu_set_errno(EPERM);
        return -1;
    }
    t->uid = (uint32_t) uid;
    return 0;
}

int64_t k_setgid(int64_t gid)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (t->gid != 0 && (uint32_t) gid != t->gid) {
        cpu_set_errno(EPERM);
        return -1;
    }
    t->gid = (uint32_t) gid;
    return 0;
}

int64_t k_umask(int64_t mask)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    uint32_t old = t->umask;
    t->umask = (uint32_t) mask & 0777u;
    return (int64_t) old;
}

int64_t k_chdir(char *dir)
{
    process_t *t = sched_get_current_process();
    char *kdir = NULL;
    char *fullpath = NULL;
    char *parent = NULL;
    char *currdir = NULL;
    uint64_t max;

    cpu_set_errno(0);

    kdir = copy_user_path_alloc(dir);
    if (kdir == NULL) {
        cpu_set_errno(EFAULT);
        goto err_exit;
    }
    dir = kdir;

    while (*dir == ' ') {
        dir++;
    }

    if (strlen(dir) == 0) {
        cpu_set_errno(ENOENT);
        goto err_exit;
    }

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    max = strlen(dir) + strlen(t->cwd) + 2;
    fullpath = kmalloc(max);
    parent = kmalloc(max);
    currdir = kmalloc(max);
    if (fullpath == NULL || parent == NULL || currdir == NULL) {
        cpu_set_errno(ENOMEM);
        goto err_exit;
    }
    fullpath[0] = '\0';
    parent[0] = '\0';
    currdir[0] = '\0';

    uint64_t k = 0;
    uint64_t len = strlen(dir);

    snprintf(fullpath, max, "%s", t->cwd);

    /* Note that "i" will loop to last character "\0" */
    for (uint64_t i = 0; i < len; i++) {
        if (dir[i] != '/') {
            currdir[k++] = dir[i];
            if (i != len - 1)
                continue;
        }
        currdir[k] = '\0';

        /* "currdir" stores current folder name */
        if (strcmp(currdir, ".") == 0) {
            /* It is current folder, do nothing */
        } else if (strcmp(currdir, "..") == 0) {
            /* Change to parent folder. The root has no parent, so stay
             * there. */
            if (vfs_get_parent_dir(fullpath, parent, currdir) < 0)
                snprintf(fullpath, max, "%s", "/");
            else
                snprintf(fullpath, max, "%s", parent);
        } else if (strlen(currdir) == 0 && i == 0) {
            /* It is root folder based */
            snprintf(fullpath, max, "%s", "/");
        } else {
            uint64_t fpl = strlen(fullpath);

            if (fpl > 0) {
                if (fullpath[fpl - 1] != '/')
                    strncat(fullpath, "/", max - fpl - 1);
            }
            strncat(fullpath, currdir, max - strlen(fullpath) - 1);
        }

        /* Set "currdir" to zero length */
        k = 0;
    }

    klogd("k_chdir: current \"%s\", target \"%s\" and change to \"%s\"\n",
          t->cwd, kdir, fullpath);

    /* The VFS server owns the namespace; ask it whether the folder exists. */
    {
        vfs_stat_t st;

        memset(&st, 0, sizeof(st));
        if (vfs_stat_path("/", fullpath, &st) < 0
            || (st.st_mode & S_IFMT) != S_IFDIR) {
            cpu_set_errno(ENOENT);
            goto err_exit;
        }
    }

    process_set_cwd(t, fullpath);

    kmfree(kdir);
    kmfree(fullpath);
    kmfree(parent);
    kmfree(currdir);
    return 0;
  err_exit:
    if (kdir != NULL)
        kmfree(kdir);
    if (fullpath != NULL)
        kmfree(fullpath);
    if (parent != NULL)
        kmfree(parent);
    if (currdir != NULL)
        kmfree(currdir);
    return -1;
}


int64_t k_fchdir(int64_t fd)
{
    process_t *t = sched_get_current_process();
    char path[VFS_MAX_PATH_LEN];
    int64_t n;

    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        return -1;
    }

    n = vfs_fd_path((int32_t) fd, path, sizeof(path));
    if (n <= 0) {
        cpu_set_errno(ENOTDIR);
        return -1;
    }

    path[n - 1] = '\0';
    process_set_cwd(t, path);
    return 0;
}

int64_t k_readdir(int64_t fd, uint64_t buff)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    int64_t errno = 0;

    cpu_set_errno(errno);

    if (desc == NULL) {
        errno = EINVAL;
        goto err_exit;
    }

    if (desc->server) {
        dirent_t de;

        memset(&de, 0, sizeof(de));

        if (desc->svc == SVC_FAT) {
            char name[256];
            uint64_t size = 0;
            bool is_dir = false;

            if (fat32_readdir_fd(desc->server_fd, desc->curr_dir_idx, name,
                                 sizeof(name), &size, &is_dir) != 0) {
                cpu_set_errno(0);
                return 0;       /* end of directory */
            }
            de.d_ino = desc->curr_dir_idx + 1;
            de.d_type = is_dir ? DT_DIR : DT_REG;
            strncpy(de.d_name, name, sizeof(de.d_name) - 1);
            desc->curr_dir_idx++;
        } else if (desc->svc == SVC_EXT) {
            char name[256];
            uint64_t size = 0;
            bool is_dir = false;

            if (ext2_readdir_fd(desc->server_fd, desc->curr_dir_idx, name,
                                sizeof(name), &size, &is_dir) != 0) {
                cpu_set_errno(0);
                return 0;       /* end of directory */
            }
            de.d_ino = desc->curr_dir_idx + 1;
            de.d_type = is_dir ? DT_DIR : DT_REG;
            strncpy(de.d_name, name, sizeof(de.d_name) - 1);
            desc->curr_dir_idx++;
        } else if (vfs_server_readdir(fd, &de) < 0) {
            /* End of directory or a server error. */
            cpu_set_errno(0);
            return 0;
        }

        if (copy_to_user((void *) buff, &de, sizeof(de)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        return (int64_t) sizeof(de);
    }

    errno = ENOTDIR;
  err_exit:
    cpu_set_errno(errno);
    return -1;
}

int64_t k_mem_stats(void *out)
{
    cpu_set_errno(0);

    if (out == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    mm_stats_t st = {
        .total = pmm_get_total_bytes(),
        .free = pmm_get_free_bytes(),
    };

    if (copy_to_user(out, &st, sizeof(st)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_meminfo()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        return -1;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    if (router_lookup(SVC_MM) == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = MM_MEMINFO;

    if (!router_forward(SVC_MM, &req, &rep) || (int64_t) rep.words[0] < 0) {
        cpu_set_errno(EIO);
        return -1;
    }

    uint64_t total = rep.words[1], free = rep.words[2], used = total - free;

    kprintf("Physical memory usage:\n"
            "  Total: %d KB (%d MB)\n"
            "  Free : %d KB (%d MB)\n"
            "  Used : %d KB (%d MB)\n",
            total / 1024, total / (1024 * 1024),
            free / 1024, free / (1024 * 1024),
            used / 1024, used / (1024 * 1024));

    return 0;
}

int64_t k_pipe(int32_t * fd, uint32_t flags)
{
    (void) flags;

    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    if (router_lookup(SVC_PIPE) != NULL) {
        ipc_msg_t req;
        ipc_msg_t rep;

        memset(&req, 0, sizeof(req));
        req.tag = PIPE_CREATE;

        if (!router_forward_timeout(SVC_PIPE, &req, &rep, 5000)
            || (int64_t) rep.words[0] < 0) {
            cpu_set_errno(ENOMEM);
            return -1;
        }

        vfs_fd_t rfd = vfs_open_server_svc((int64_t) rep.words[1],
                                           "/dev/pipe", VFS_MODE_READ, 0,
                                           SVC_PIPE);
        vfs_fd_t wfd = vfs_open_server_svc((int64_t) rep.words[2],
                                           "/dev/pipe", VFS_MODE_WRITE, 0,
                                           SVC_PIPE);

        if (rfd == VFS_INVALID_FD || wfd == VFS_INVALID_FD) {
            cpu_set_errno(ENOMEM);
            return -1;
        }

        int32_t kfds[2] = { (int32_t) rfd, (int32_t) wfd };

        if (fd == NULL || copy_to_user(fd, kfds, sizeof(kfds)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }

        klogi("k_pipe: server pipe read %ld write %ld\n", (int64_t) rfd,
              (int64_t) wfd);
        return 0;
    }

    /* Pipes are only served from userspace. */
    cpu_set_errno(ENOSYS);

  err_exit:
    return -1;
}



int64_t k_fsync(int64_t fd)
{
    (void)fd;
    cpu_set_errno(0);
    return 0;
}

int64_t k_truncate(char *path, int64_t len)
{
    if (len < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    char *kpath = copy_user_path_alloc(path);

    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    int32_t svc = SVC_FS;
    vfs_fd_t fd = vfs_open_path(cwd, kpath, 0, &svc);

    kmfree(kpath);
    if (fd == VFS_INVALID_FD) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    int64_t r = vfs_truncate(fd, (uint64_t) len);

    vfs_close(fd);

    if (r < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}

static int64_t memfd_truncate(int64_t id, uint64_t len)
{
    memfd_obj_t *mf = memfd_get(id);

    if (mf == NULL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    len = (len + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);

    if (len == 0) {
        if (mf->mo != NULL) {
            memobj_unref(mf->mo);
            mf->mo = NULL;
        }
        mf->size = 0;
        cpu_set_errno(0);
        return 0;
    }

    if (mf->mo != NULL && memobj_size(mf->mo) == len) {
        mf->size = len;
        cpu_set_errno(0);
        return 0;
    }

    memobj_t *nw = memobj_create(len);

    if (nw == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    if (mf->mo != NULL) {
        uint64_t old = memobj_size(mf->mo);
        uint64_t n = old < len ? old : len;
        uint8_t *tmp = kmalloc(PAGE_SIZE);

        if (tmp == NULL) {
            memobj_unref(nw);
            cpu_set_errno(ENOMEM);
            return -1;
        }
        for (uint64_t off = 0; off < n; off += PAGE_SIZE) {
            uint64_t chunk = (n - off < PAGE_SIZE) ? n - off : PAGE_SIZE;

            memobj_copy_out(mf->mo, tmp, chunk, off);
            memobj_copy_in(nw, tmp, chunk, off);
        }
        kmfree(tmp);
        memobj_unref(mf->mo);
    }

    mf->mo = nw;
    mf->size = len;
    cpu_set_errno(0);
    return 0;
}

int64_t k_ftruncate(int64_t fd, int64_t len)
{
    if (len < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) == 0
        && svc == SVC_MEMFD)
        return memfd_truncate(sfd, (uint64_t) len);

    int64_t r = vfs_truncate((vfs_fd_t) fd, (uint64_t) len);

    if (r < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}

/* Copy the kernel log ring to userspace so a shell can show it (dmesg). The
 * physical machine usually has no serial console to read the boot log from. */
int64_t k_klog(char *ubuf, uint64_t len)
{
    if (ubuf == NULL || len == 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (len > KLOG_BUFFER_SIZE)
        len = KLOG_BUFFER_SIZE;

    char *kbuf = kmalloc(len);

    if (kbuf == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }
    uint64_t n = klog_read(kbuf, len);
    int32_t rc = (int32_t) copy_to_user(ubuf, kbuf, n);

    kmfree(kbuf);
    if (rc != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    cpu_set_errno(0);
    return (int64_t) n;
}

#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLNVAL    0x020

static signalfd_obj_t *signalfd_get(int64_t id)
{
    if (id < 1 || id > SIGNALFD_MAX)
        return NULL;

    return &signal_fds[id - 1];
}

static eventfd_obj_t *eventfd_get(int64_t id)
{
    if (id < 1 || id > EVENTFD_MAX || !eventfds[id - 1].used)
        return NULL;
    return &eventfds[id - 1];
}

static epoll_obj_t *epoll_get(int64_t id)
{
    if (id < 1 || id > EPOLL_MAX || !epolls[id - 1].used)
        return NULL;
    return &epolls[id - 1];
}

static bool fd_is_valid(int32_t fd)
{
    if (fd < 0)
        return false;
    if (fd < 3)
        return true;
    return process_fd_get(fd, NULL, NULL, NULL, NULL, NULL) == 0;
}

/** @brief Kernel copy of struct pollfd */
struct pollfd_k {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

/* Readiness of one descriptor for the given events. Regular files and devices
 * report ready; a pipe asks its server; stdin asks the tty. Set *want_tty when
 * a polled stdin has no key, so the caller blocks on the tty wake key. */
static int16_t fd_poll_revents(int32_t fd, int16_t events, bool *want_tty)
{
    if (fd < 0)
        return 0;

    if (fd == 0) {
        int16_t rev = 0;

        if (events & POLLIN) {
            if (tty_server_pending() > 0)
                rev |= POLLIN;
            else if (want_tty != NULL)
                *want_tty = true;
        }
        if (events & POLLOUT)
            rev |= POLLOUT;
        return rev;
    }

    if (fd <= 2)
        return (events & POLLOUT) ? POLLOUT : 0;

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (process_fd_get(fd, &kind, &svc, &sfd, NULL, NULL) != 0)
        return POLLNVAL;

    int16_t rev = 0;

    if (svc == SVC_PIPE) {
        int32_t rd = 0, wr = 0;

        if (vfs_pipe_poll(sfd, &rd, &wr) != 0)
            return POLLNVAL;
        if ((events & POLLIN) && rd)
            rev |= POLLIN;
        if ((events & POLLOUT) && wr)
            rev |= POLLOUT;
        return rev;
    }

    if (svc == SVC_FS) {
        int32_t rd = 0, wr = 0;

        if (vfs_fifo_poll(sfd, &rd, &wr) != 0)
            return POLLNVAL;
        if ((events & POLLIN) && rd)
            rev |= POLLIN;
        if ((events & POLLOUT) && wr)
            rev |= POLLOUT;
        return rev;
    }

    if (svc == SVC_NET) {
        int32_t rd = 0, wr = 0;

        if (net_poll((int32_t) fd, &rd, &wr) != 0)
            return POLLNVAL;
        if ((events & POLLIN) && rd)
            rev |= POLLIN;
        if ((events & POLLOUT) && wr)
            rev |= POLLOUT;
        return rev;
    }

    if (svc == SVC_EVENT) {
        eventfd_obj_t *e = eventfd_get(sfd);
        uint64_t c;

        if (e == NULL)
            return POLLNVAL;

        kobj_lock_acquire();
        c = e->counter;
        kobj_lock_release();

        if ((events & POLLIN) && c > 0)
            rev |= POLLIN;
        if ((events & POLLOUT) && c != ~0ULL)
            rev |= POLLOUT;
        return rev;
    }

    if (svc == SVC_SIGNALFD) {
        signalfd_obj_t *sf = signalfd_get(sfd);
        process_t *cur = sched_get_current_process();

        if (sf == NULL || cur == NULL)
            return POLLNVAL;

        if (events & POLLIN) {
            spinlock_acquire(&cur->signals.lock);
            uint64_t ready = cur->signals.pending.sig & sf->mask
                & cur->signals.mask.sig;
            spinlock_release(&cur->signals.lock);
            if (ready)
                rev |= POLLIN;
        }
        return rev;
    }

    if (svc == SVC_EPOLL)
        return 0;

    if (events & POLLIN)
        rev |= POLLIN;
    if (events & POLLOUT)
        rev |= POLLOUT;
    return rev;
}

/* Report readiness once. Returns the ready count, or -1 on a bad pointer. Set
 * *want_tty when a polled stdin needs the tty to wake a blocking caller. */
static int32_t poll_check(void *fds, uint64_t nfds, bool *want_tty)
{
    int32_t ready = 0;

    if (want_tty != NULL)
        *want_tty = false;

    for (uint64_t i = 0; i < nfds; i++) {
        struct pollfd_k p;
        uint8_t *slot = (uint8_t *) fds + i * sizeof(p);

        if (copy_from_user(&p, slot, sizeof(p)) != 0)
            return -1;

        int16_t rev = fd_poll_revents(p.fd, p.events, want_tty);

        p.revents = rev;

        if (copy_to_user(slot, &p, sizeof(p)) != 0)
            return -1;
        if (rev)
            ready++;
    }

    return ready;
}

/* Register a poll wake key with one descriptor for the given event bits. */
static void poll_register_one(int32_t fd, void *key, int32_t events)
{
    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (key == NULL || fd < 3)
        return;
    if (process_fd_get(fd, &kind, &svc, &sfd, NULL, NULL) != 0)
        return;

    if (svc == SVC_PIPE) {
        vfs_pipe_poll_register(sfd, key, events);
    } else if (svc == SVC_FS) {
        vfs_fifo_poll_register(sfd, key, events);
    } else if (svc == SVC_NET) {
        net_poll_register(fd, key, events);
    } else if (svc == SVC_EVENT) {
        kobj_lock_acquire();
        eventfd_obj_t *e = eventfd_get(sfd);

        if (e != NULL) {
            e->wait_key = key;
            e->wait_events = events;
        }
        kobj_lock_release();
    }
}

/* Register a poll wake key with each non-tty polled descriptor, so its server
 * or kernel object wakes us when the state changes. */
static void poll_register(void *fds, uint64_t nfds, void *key)
{
    if (key == NULL)
        return;

    for (uint64_t i = 0; i < nfds; i++) {
        struct pollfd_k p;
        uint8_t *slot = (uint8_t *) fds + i * sizeof(p);

        if (copy_from_user(&p, slot, sizeof(p)) != 0)
            return;
        poll_register_one(p.fd, key, p.events);
    }
}

int64_t k_poll(void *fds, uint64_t nfds, int32_t timeout)
{
    int64_t start_ms = (int64_t) (hpet_get_nanos() / 1000000);

    cpu_set_errno(0);

    if (fds == NULL || nfds > 1024) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    process_t *t = sched_get_current_process();

    for (;;) {
        bool want_tty = false;
        int32_t ready = poll_check(fds, nfds, &want_tty);

        if (ready < 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        if (ready > 0 || timeout == 0)
            return ready;

        int64_t rem = -1;

        if (timeout > 0) {
            int64_t elapsed =
                (int64_t) (hpet_get_nanos() / 1000000) - start_ms;

            if (elapsed >= timeout)
                return 0;
            rem = timeout - elapsed;
        }

        void *key = want_tty ? tty_server_poll_key()
            : (t != NULL ? (void *) &t->poll_key : NULL);

        /* Register first: poll_register and poll_check may block on IPC, and
         * the IPC reply wait reuses the process wake key. Arm the poll key
         * only after them so the server's wake still matches it. */
        poll_register(fds, nfds, key);

        bool again = false;

        ready = poll_check(fds, nfds, &again);
        if (ready > 0)
            return ready;

        if (key != NULL) {
            sched_wait_key_begin(key);
            if (timeout < 0)
                sched_wait_key_commit_infinite();
            else
                sched_wait_key_commit(rem);
        } else {
            sched_sleep(timeout < 0 ? 1000 : rem);
        }
    }
}

int64_t k_poll_wake(uint64_t key)
{
    cpu_set_errno(0);
    sched_wake_key((void *) key);
    return 0;
}

static int64_t select_common(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                             uint8_t * efds, int64_t timeout_ms)
{
    uint8_t in[3][128] = { {0} };
    uint8_t out[3][128] = { {0} };
    uint8_t *sets[3] = { rfds, wfds, efds };
    uint64_t bytes;

    cpu_set_errno(0);

    if (nfds < 0 || nfds > 1024) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    bytes = ((uint64_t) nfds + 7) / 8;

    for (int32_t s = 0; s < 3; s++) {
        if (sets[s] != NULL && bytes > 0
            && copy_from_user(in[s], sets[s], bytes) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    int32_t ready = 0;

    for (int32_t fd = 0; fd < nfds; fd++) {
        int32_t bit = fd / 8;
        uint8_t mask = (uint8_t) (1 << (fd % 8));
        bool any = false;

        if (!fd_is_valid(fd))
            continue;

        if (rfds != NULL && (in[0][bit] & mask)
            && (fd_poll_revents(fd, POLLIN, NULL) & POLLIN)) {
            out[0][bit] |= mask;
            any = true;
        }
        if (wfds != NULL && (in[1][bit] & mask)
            && (fd_poll_revents(fd, POLLOUT, NULL) & POLLOUT)) {
            out[1][bit] |= mask;
            any = true;
        }
        if (efds != NULL && (in[2][bit] & mask)) {
            out[2][bit] |= mask;
            any = true;
        }
        if (any)
            ready++;
    }

    for (int32_t s = 0; s < 3; s++) {
        if (sets[s] != NULL && bytes > 0
            && copy_to_user(sets[s], out[s], bytes) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    if (ready == 0 && timeout_ms > 0)
        sched_sleep(timeout_ms);

    return ready;
}

int64_t k_select(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                 uint8_t * efds, void *tv)
{
    int64_t ms = 0;

    cpu_set_errno(0);

    if (tv != NULL) {
        struct {
            int64_t sec;
            int64_t usec;
        } ktv;

        if (copy_from_user(&ktv, tv, sizeof(ktv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        ms = ktv.sec * 1000 + (ktv.usec + 999) / 1000;
    }

    return select_common(nfds, rfds, wfds, efds, ms);
}

int64_t k_pselect6(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                   uint8_t * efds, void *tsp, void *sigmask)
{
    int64_t ms = 0;

    (void)sigmask;
    cpu_set_errno(0);

    if (tsp != NULL) {
        vfs_timespec_t ts = { 0, 0 };

        if (copy_from_user(&ts, tsp, sizeof(ts)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
    }

    return select_common(nfds, rfds, wfds, efds, ms);
}

int64_t k_sched_yield(void)
{
    cpu_set_errno(0);
    sched_sleep(0);
    return 0;
}

int64_t k_prlimit64(int32_t pid, int32_t resource, void *newlim, void *oldlim)
{
    struct {
        uint64_t cur;
        uint64_t max;
    } lim = { ~0ULL, ~0ULL };

    /* Only reading is supported: report RLIM_INFINITY. */
    (void)pid;
    (void)resource;
    (void)newlim;

    cpu_set_errno(0);

    if (oldlim != NULL
        && copy_to_user(oldlim, &lim, sizeof(lim)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_clone(uint64_t flags, uint64_t stack, int32_t * ptid,
                int32_t * ctid, uint64_t tls)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);


    if (t == NULL || t->mode != PROC_USER_MODE || t->addrspace == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    /* Only thread creation shares the address space. */
    if (!(flags & CLONE_VM)) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL || cpu->syscall_frame == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    process_t *tc = process_clone(t, flags, stack, tls, ctid,
                                  cpu->syscall_frame);
    if (tc == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    pid_t tid = tc->pid;

    if ((flags & CLONE_PARENT_SETTID) && ptid != NULL)
        copy_to_user(ptid, &tid, sizeof(tid));
    if ((flags & CLONE_CHILD_SETTID) && ctid != NULL)
        copy_to_user(ctid, &tid, sizeof(tid));

    sched_add(tc);
    return tid;
}

int64_t k_fork()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    pid_t tid_child = sched_fork();
    process_t *curr_proc = sched_get_current_process();

    klogd("k_fork: parent process id #%ld, current process id #%ld, PML4 0x%016lx, "
          "sched_fork() returns #%ld\n",
          t->pid, sched_get_pid(), curr_proc->addrspace->PML4, tid_child);

    if (tid_child == PID_MAX) {
        cpu_set_errno(ECHILD);
        return -1;
    } else if (t->pid == sched_get_pid()) {
        /* Parent: the process server clones the child's fd table, then the
         * child is allowed to run. */
        process_fd_fork((int32_t) t->pid, (int32_t) tid_child);
        sched_mark_fds_ready(tid_child);
        klogd("k_fork: return %ld from parent process #%ld\n", tid_child,
              t->pid);
        return tid_child;
    } else {
        /* Child: wait until the parent has cloned our fd table. */
        klogd("k_fork: return 0 from child process #%ld\n", tid_child);
        sched_wait_fds_ready(curr_proc);
        return 0;
    }
  err_exit:
    return -1;
}

int64_t k_getppid()
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);
    return (t != NULL) ? t->ppid : 0;
}

static int64_t current_pgrp(void)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);
    return (t != NULL) ? (int64_t) t->pid : -1;
}

int64_t k_getpgrp(void)
{
    return current_pgrp();
}

int64_t k_getpgid(pid_t pid)
{
    if (pid == 0)
        return current_pgrp();
    return (process_lookup(pid) != NULL) ? (int64_t) pid : -1;
}

int64_t k_setpgid(pid_t pid, pid_t pgid)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);
    if (t == NULL || (pid != 0 && pid != t->pid)
        || (pgid != 0 && pgid != t->pid)) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    return 0;
}

int64_t k_setsid(void)
{
    return current_pgrp();
}

int64_t k_getsid(pid_t pid)
{
    if (pid == 0)
        return current_pgrp();
    return (process_lookup(pid) != NULL) ? (int64_t) pid : -1;
}

int64_t k_dup3(int64_t oldfd, int64_t newfd, int64_t flags);

int64_t k_fcntl(int64_t fd, int64_t request, int64_t arg)
{
    cpu_set_errno(0);

    /* F_DUPFD and F_DUPFD_CLOEXEC duplicate to the lowest free descriptor. */
    if (request == F_DUPFD || request == F_DUPFD_CLOEXEC) {
        int64_t r = k_dup3(fd, -1,
                           (request == F_DUPFD_CLOEXEC) ? O_CLOEXEC : 0);

        if (r < 0) {
            cpu_set_errno(EBADF);
            return -1;
        }
        return r;
    }

    if (request == F_GETFL) {
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;

        if (!(fd >= 0 && fd < 3)
            && process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) != 0) {
            cpu_set_errno(EBADF);
            return -1;
        }
        return O_RDWR;
    }

    if (request == F_SETFL)
        return 0;

    /* Only the descriptor flags live in the process server for now. */
    if (request == F_GETFD || request == F_SETFD) {
        int64_t r = process_fd_fcntl((int32_t) fd, (int32_t) request, arg);

        if (r < 0) {
            /* The standard descriptors are implicit tty fds until a
             * redirection installs an entry for them. */
            if (fd >= 0 && fd < 3)
                return 0;
            cpu_set_errno(EBADF);
            return -1;
        }
        return r;
    }

    cpu_set_errno(ENOSYS);
    return -1;
}

int64_t k_waitpid(int64_t pid, int32_t * status, int32_t flags)
{
    process_t *parent = sched_get_current_process();

    if (status != NULL && clear_user(status, sizeof(*status)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (parent == NULL) {
        cpu_set_errno(ECHILD);
        return -1;
    }

    /* pid is passed as a 32-bit signed value, so truncate before comparing:
     * a userland wait(-1) arrives as 0x00000000FFFFFFFF. */
    int32_t wpid = (int32_t) pid;
    bool nohang = (flags & WNOHANG) != 0;

    while (true) {
        int64_t st = 0;
        int64_t r = process_wait((int32_t) wpid, nohang ? 1 : 0, &st);

        if (r > 0) {
            if (status != NULL) {
                int32_t st32 = (int32_t) st;
                if (copy_to_user(status, &st32, sizeof(st32)) != 0) {
                    cpu_set_errno(EFAULT);
                    return -1;
                }
            }
            cpu_set_errno(0);
            return r;
        }

        if (r == 0) {           /* WNOHANG: a child exists but has not exited */
            cpu_set_errno(0);
            return 0;
        }

        if (r == PROC_WAIT_BLOCK) {
            /* A child exists but has not exited. The server records the status;
             * sleep briefly and ask again. */
            sched_wait_child(10);
            continue;
        }

        cpu_set_errno(ECHILD);
        return -1;
    }
}

/* Close the process on the server side and record its wait status. The kernel
 * stores the Linux wait status directly: a normal exit shifts the low byte up,
 * a signal death keeps the signal in the low bits. */
static void process_record_exit(process_t * t, int64_t wait_status)
{
    process_fd_exit((int32_t) t->pid);
    process_exit_notify(wait_status);
    t->exit_status = wait_status;
    t->exit_recorded = true;
}

void k_exit(int64_t status)
{
    process_t *t = sched_get_current_process();
    if (t != NULL) {
        if (t->is_thread) {
            klogi("k_exit: thread %ld in group %ld exit\n", t->pid, t->tgid);
            /* CLONE_CHILD_CLEARTID: release the joiner. */
            if (t->clear_child_tid != NULL) {
                int32_t zero = 0;

                copy_to_user(t->clear_child_tid, &zero, sizeof(zero));
                sched_wake_key((void *) t->clear_child_tid);
            }
        } else {
            klogi("k_exit: process %ld exit with status %ld\n", t->pid,
                  status);
            /* The process server owns the fd table; record the wait status so
             * the parent's wait can collect it. */
            process_record_exit(t, (status & 0xff) << 8);
        }
    }

    /* Exit from scheduler */
    sched_exit(status);
}

/* Record a group exit under the leader's pid. A non-leader thread that calls
 * exit_group must still answer the parent's wait on the leader. Close the
 * group fd table and store the status once; the reaper skips a recorded
 * process. */
static void exit_group_record(process_t * t, int64_t wait_status)
{
    process_t *leader = process_lookup(t->tgid);

    if (leader == NULL || leader->exit_recorded)
        return;

    process_fd_exit_pid((int32_t) leader->pid);
    mmap_file_cache_drop_pid(leader->pid);
    process_exit_notify_pid((int32_t) leader->pid, wait_status);
    leader->exit_status = wait_status;
    leader->exit_recorded = true;
}

void k_exit_group(int64_t status)
{
    process_t *t = sched_get_current_process();

    if (t != NULL) {
        klogi("k_exit_group: group %ld exit with status %ld\n", t->tgid,
              status);
        exit_group_record(t, (status & 0xff) << 8);
        /* Stop the other threads of the group after the status is stored. */
        sched_kill_group(t->tgid, t->pid);
    }

    sched_exit(status);
}

/* Exit the group because of a fatal signal. The wait status carries the signal
 * in the low bits with the core-dump flag set. */
void k_exit_group_signal(int32_t sig)
{
    process_t *t = sched_get_current_process();

    if (t != NULL) {
        exit_group_record(t, (int64_t) sig | 0x80);
        sched_kill_group(t->tgid, t->pid);
    }

    sched_exit(sig);
}

int32_t k_getcwd(char *buffer, uint64_t size)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buffer == NULL || size <= 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    uint64_t len = strlen(t->cwd);
    if (len < size - 1) {
        if (copy_to_user(buffer, t->cwd, len + 1) != 0) {
            cpu_set_errno(EFAULT);
            goto err_exit;
        }
    } else {
        cpu_set_errno(ENAMETOOLONG);
        goto err_exit;
    }

    return 0;

  err_exit:
    return -1;
}

int32_t k_getrusage(int64_t who, uint64_t usage)
{
    /* When gcc is launched, it will call getrusage(). We need to dive into
     * gcc to know the purpose of this function call.
     */
    klogw("SYSCALL: get 0x%016lx rusage\n", who);

    if (clear_user((void *) usage, sizeof(rusage_t)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

/* Pack path/cwd/argv/envp and the ELF file into one buffer and hand it to the
 * process server, which parses and maps it. Returns 0 on success. */
static int32_t exec_via_server(process_t * t, const char *kpath,
                           const char **argv, const char **envp,
                           const char *cwd)
{
    uint64_t argc = 0, envc = 0, i;

    while (argv != NULL && argv[argc] != NULL && argc < EXEC_MAX_ARGS)
        argc++;
    while (envp != NULL && envp[envc] != NULL && envc < EXEC_MAX_ARGS)
        envc++;

    uint8_t *elf = NULL;
    uint64_t elf_len = 0;

    if (vfs_load_file(kpath, &elf, &elf_len) != 0 || elf == NULL) {
        kloge("k_execve: cannot read \"%s\"\n", kpath);
        return -1;
    }

    uint64_t header = 8 * sizeof(uint64_t);
    uint64_t path_len = strlen(kpath) + 1;
    uint64_t cwd_len = ((cwd != NULL) ? strlen(cwd) : 0) + 1;
    uint64_t off_argv = header + path_len + cwd_len;
    uint64_t off_envp = off_argv + argc * sizeof(uint64_t);
    uint64_t off_strs = off_envp + envc * sizeof(uint64_t);

    uint64_t strs = 0;

    for (i = 0; i < argc; i++)
        strs += strlen(argv[i]) + 1;
    for (i = 0; i < envc; i++)
        strs += strlen(envp[i]) + 1;

    uint64_t off_elf = off_strs + strs;
    uint64_t total = off_elf + elf_len;
    uint8_t *buf = kmalloc(total);

    if (buf == NULL) {
        kmfree_pages(elf, __func__, __LINE__);
        return -1;
    }

    uint64_t *h = (uint64_t *) buf;

    memset(buf, 0, header);
    h[0] = header;
    h[1] = header + path_len;
    h[2] = off_argv;
    h[3] = off_envp;
    h[4] = argc;
    h[5] = envc;
    h[6] = off_elf;
    h[7] = elf_len;
    memcpy(buf + header, kpath, path_len);
    memcpy(buf + header + path_len, (cwd != NULL) ? cwd : "", cwd_len);

    uint64_t *av = (uint64_t *) (buf + off_argv);
    uint64_t *ev = (uint64_t *) (buf + off_envp);
    uint8_t *p = buf + off_strs;

    for (i = 0; i < argc; i++) {
        uint64_t n = strlen(argv[i]) + 1;
        memcpy(p, argv[i], n);
        av[i] = (uint64_t) (p - buf);
        p += n;
    }
    for (i = 0; i < envc; i++) {
        uint64_t n = strlen(envp[i]) + 1;
        memcpy(p, envp[i], n);
        ev[i] = (uint64_t) (p - buf);
        p += n;
    }
    memcpy(buf + off_elf, elf, elf_len);
    kmfree_pages(elf, __func__, __LINE__);

    handle_t bh = 0;

    if (ipc_buf_from_kernel(buf, total, &bh) != 0) {
        kloge("k_execve: ipc_buf_from_kernel failed\n");
        kmfree(buf);
        return -1;
    }
    kmfree(buf);

    ipc_msg_t req, rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.tag = PROC_EXEC;
    req.words[0] = (uint64_t) t->pid;
    req.xfer[0] = bh;
    req.xfer_count = 1;

    if (!router_forward_timeout(SVC_PROC, &req, &rep, 15000)) {
        kloge("k_execve: router_forward(SVC_PROC) failed\n");
        return -1;
    }
    if ((int64_t) rep.words[0] < 0) {
        kloge("k_execve: process server returned %ld\n",
              (int64_t) rep.words[0]);
        return -1;
    }
    return 0;
}

int64_t k_execve(const char *path, const char *argv[], const char *envp[])
{
    process_t *tcache = sched_get_current_process();

    if (tcache != NULL) {
        tcache->at_dirfd = -1;
        tcache->fd_cache_fd = -1;
    }

    char *cwd = NULL;
    process_t *t = sched_get_current_process();
    if (t != NULL)
        cwd = t->cwd;

    char *kpath = copy_user_path_alloc(path);
    if (kpath == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    char *kargv[EXEC_MAX_ARGS + 1];
    char *kenvp[EXEC_MAX_ARGS + 1];

    if (copy_exec_argv(argv, kargv) != 0
        || copy_exec_argv(envp, kenvp) != 0) {
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        kmfree(kpath);
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char **kargv_p = (argv != NULL) ? (const char **) kargv : NULL;
    const char **kenvp_p = (envp != NULL) ? (const char **) kenvp : NULL;

    if (t != NULL && t->mode == PROC_USER_MODE) {
        int32_t r = sched_execve_inplace(kpath, kargv_p, kenvp_p, cwd);

        if (r == 0) {
            process_fd_close_cloexec();
            klogi("k_execve: inplace \"%s\" in process %ld\n", kpath, t->pid);
            free_exec_argv(kargv);
            free_exec_argv(kenvp);
            kmfree(kpath);
            cpu_set_errno(0);
            return 0;
        }
    }

    /* The process server owns process creation. When it is registered, do not
     * fall back to the in-kernel loader on failure: that would create a second
     * process for one exec while the server may have already spawned one. */
    if (t != NULL && router_lookup(SVC_PROC) != NULL) {
        if (exec_via_server(t, kpath, kargv_p, kenvp_p, cwd) == 0) {
            klogi("k_execve: server ran \"%s\", exit process %ld\n", kpath,
                  t->pid);
            free_exec_argv(kargv);
            free_exec_argv(kenvp);
            process_fd_exit((int32_t) t->pid);
            process_exit_notify(0);
            sched_exit(0);
            cpu_set_errno(0);
            return 0;
        }

        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        kmfree(kpath);
        cpu_set_errno(ENOEXEC);
        return -1;
    }

    if (sched_execve(kpath, kargv_p, kenvp_p, cwd) != NULL) {
        klogi("k_execve: run \"%s\" and exit from process %ld\n", kpath,
              t != NULL ? t->pid : 0);
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        process_fd_exit((int32_t) (t != NULL ? t->pid : 0));
        sched_exit(0);
        cpu_set_errno(0);
        return 0;
    }

    /* Report the reason for the failure: a missing execute bit is a
     * permission error, not a malformed image. */
    int err = EINVAL;
    vfs_stat_t st;

    if (vfs_stat_path(cwd, kpath, &st) == 0) {
        if ((st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
            err = EACCES;
    } else {
        err = ENOENT;
    }

    free_exec_argv(kargv);
    free_exec_argv(kenvp);
    kmfree(kpath);
    cpu_set_errno(err);
    return -1;
}


/* Allocate a memory object and a handle for the misc server to fill or read.
 * The caller keeps the creation reference so it can copy the result out after
 * the forward. */



int64_t k_dup3(int64_t oldfd, int64_t newfd, int64_t flags)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    /* A replaced descriptor may hold buffered server writes; flush them before
     * the dup drops the server-side description. */
    if (newfd >= 0) {
        int32_t ckind = 0, csvc = 0;
        int64_t csfd = 0;

        if (process_fd_get((int32_t) newfd, &ckind, &csvc, &csfd, NULL, NULL) == 0)
            vfs_fd_cache_drop(csvc, csfd);
    }

    /* Linux dup3(oldfd, newfd, flags): make newfd refer to oldfd. newfd < 0
     * means the caller wants the lowest free descriptor (dup). */
    int64_t r = process_fd_dup((int32_t) oldfd, (int32_t) newfd);

    /* The standard descriptors live in the kernel, not the process server.
     * Back a duplicate of them with the tty server. */
    if (r < 0 && oldfd >= 0 && oldfd < 3) {
        vfs_openmode_t mode =
            (oldfd == 0) ? VFS_MODE_READ : VFS_MODE_WRITE;
        int64_t nfd = vfs_open_server_svc(oldfd, "/dev/tty", mode, 0,
                                          SVC_TTY);

        if (nfd >= 0 && newfd >= 0) {
            int64_t r2 = process_fd_dup((int32_t) nfd, (int32_t) newfd);

            process_fd_close((int32_t) nfd, NULL, NULL, NULL);
            r = r2;
        } else {
            r = nfd;
        }
    }

    if (r < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }

    if (t != NULL && r >= 0 && r == t->at_dirfd)
        t->at_dirfd = -1;

    if ((flags & O_CLOEXEC) && r >= 0)
        process_fd_fcntl((int32_t) r, F_SETFD, FD_CLOEXEC);

    return r;
}

int64_t k_futex_wait(int64_t * ptr, vfs_timespec_t * tv, int64_t expected)
{
    int64_t val = 0;
    int64_t millis = 0;

    cpu_set_errno(0);

    if (ptr == NULL || copy_from_user(&val, ptr, sizeof(val)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (val != expected) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (tv != NULL) {
        vfs_timespec_t ktv = { 0 };

        if (copy_from_user(&ktv, tv, sizeof(ktv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        millis = ktv.tv_sec * 1000 + ktv.tv_nsec / 1000000;
    }

    sched_wait_key_begin(ptr);

    /* Re-check after arming: a wake between the first check and the arm must
     * not be lost. */
    if (copy_from_user(&val, ptr, sizeof(val)) != 0) {
        sched_wait_key_cancel();
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (val != expected) {
        sched_wait_key_cancel();
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (tv == NULL)
        sched_wait_key_commit_infinite();
    else
        sched_wait_key_commit(millis);

    return 0;
}

int64_t k_futex_wake(int64_t * ptr, int64_t nr)
{
    cpu_set_errno(0);

    if (ptr == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return sched_wake_key_n(ptr, nr);
}

/* ----- Microkernel: endpoints, IPC and handles ----- */

/* Allocate a kernel object and register it as a process fd. */
static int64_t kobj_register(int32_t svc, int64_t id)
{
    return vfs_open_server_svc(id, "", VFS_MODE_READWRITE, 0, svc);
}

int64_t k_kobj_install(int32_t svc, int64_t id)
{
    return kobj_register(svc, id);
}

static int64_t k_signalfd4(int64_t fd, const void *umask, uint64_t sizemask,
                           int32_t flags)
{
    process_t *t = sched_get_current_process();
    uint64_t mask = 0;

    (void) flags;
    cpu_set_errno(0);

    if (t == NULL || umask == NULL || sizemask < sizeof(uint64_t)) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (copy_from_user(&mask, umask, sizeof(mask)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (fd == -1) {
        int32_t slot = -1;

        kobj_lock_acquire();
        for (int32_t i = 0; i < SIGNALFD_MAX; i++) {
            if (!signal_fds[i].used) {
                signal_fds[i].used = true;
                signal_fds[i].refs = 1;
                signal_fds[i].mask = mask;
                slot = i;
                break;
            }
        }
        kobj_lock_release();

        if (slot < 0) {
            cpu_set_errno(EMFILE);
            return -1;
        }

        int64_t nfd = kobj_register(SVC_SIGNALFD, slot + 1);

        if (nfd < 0) {
            kobj_lock_acquire();
            signal_fds[slot].used = false;
            kobj_lock_release();
            cpu_set_errno(EMFILE);
            return -1;
        }
        return nfd;
    }

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) != 0
        || svc != SVC_SIGNALFD) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    signalfd_obj_t *sf = signalfd_get(sfd);

    if (sf == NULL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    sf->mask = mask;
    return fd;
}

static int64_t signalfd_read(int64_t id, void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();
    signalfd_obj_t *sf = signalfd_get(id);

    if (count < sizeof(signalfd_siginfo_t) || buf == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (sf == NULL || t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    spinlock_acquire(&t->signals.lock);

    uint64_t ready = t->signals.pending.sig & sf->mask & t->signals.mask.sig;

    if (ready == 0) {
        spinlock_release(&t->signals.lock);
        cpu_set_errno(EAGAIN);
        return -1;
    }

    int32_t sig = __builtin_ctzll(ready) + 1;

    SIGNAL_SETOFF(&t->signals.pending, sig);
    spinlock_release(&t->signals.lock);

    signalfd_siginfo_t si;

    memset(&si, 0, sizeof(si));
    si.ssi_signo = (uint32_t) sig;
    si.ssi_pid = (uint32_t) t->pid;

    if (copy_to_user(buf, &si, sizeof(si)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return (int64_t) sizeof(si);
}

bool mm_selftest(void)
{
    ipc_msg_t req;
    ipc_msg_t rep;
    bool ok = false;

    if (router_lookup(SVC_MM) != NULL) {
        memset(&req, 0, sizeof(req));
        req.tag = MM_MEMINFO;
        ok = router_forward(SVC_MM, &req, &rep)
            && (int64_t) rep.words[0] >= 0 && rep.words[1] > 0;
    }

    klogi("MK: mm %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

bool signalfd_selftest(void)
{
    process_t *t = sched_get_current_process();
    bool ok = true;
    uint64_t mask = 1ul << (SIGUSR1 - 1);
    int64_t fd = -1;

    if (t == NULL)
        return false;

    sigset_t saved;

    spinlock_acquire(&t->signals.lock);
    saved = t->signals.mask;
    SIGNAL_SETON(&t->signals.mask, SIGUSR1);
    spinlock_release(&t->signals.lock);

    fd = k_signalfd4(-1, &mask, sizeof(mask), 0);

    if (fd < 0) {
        ok = false;
    } else {
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;

        signal_raise(t, SIGUSR1);
        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, NULL, NULL) != 0
            || svc != SVC_SIGNALFD) {
            ok = false;
        } else {
            signalfd_siginfo_t si;

            memset(&si, 0, sizeof(si));
            if (signalfd_read(sfd, &si, sizeof(si)) != (int64_t) sizeof(si)
                || si.ssi_signo != (uint32_t) SIGUSR1)
                ok = false;
        }
        k_close(fd);
    }

    spinlock_acquire(&t->signals.lock);
    t->signals.mask = saved;
    SIGNAL_SETOFF(&t->signals.pending, SIGUSR1);
    spinlock_release(&t->signals.lock);

    klogi("MK: signalfd %s\n", ok ? "PASS" : "FAIL");
    return ok;
}

int64_t k_eventfd2(uint32_t initval, int32_t flags)
{
    int32_t slot = -1;

    cpu_set_errno(0);

    kobj_lock_acquire();
    for (int32_t i = 0; i < EVENTFD_MAX; i++) {
        if (!eventfds[i].used) {
            eventfds[i].used = true;
            eventfds[i].refs = 1;
            eventfds[i].counter = initval;
            eventfds[i].semaphore = (flags & EFD_SEMAPHORE) != 0;
            slot = i;
            break;
        }
    }
    kobj_lock_release();

    if (slot < 0) {
        cpu_set_errno(EMFILE);
        return -1;
    }

    int64_t fd = kobj_register(SVC_EVENT, slot + 1);

    if (fd < 0) {
        kobj_lock_acquire();
        eventfds[slot].used = false;
        kobj_lock_release();
        cpu_set_errno(EMFILE);
        return -1;
    }
    return fd;
}

int64_t k_memfd_create(const char *name, uint64_t flags)
{
    int32_t slot = -1;

    (void) name;
    (void) flags;
    cpu_set_errno(0);

    kobj_lock_acquire();
    for (int32_t i = 0; i < MEMFD_MAX; i++) {
        if (!memfds[i].used) {
            memfds[i].used = true;
            memfds[i].refs = 1;
            memfds[i].mo = NULL;
            memfds[i].size = 0;
            slot = i;
            break;
        }
    }
    kobj_lock_release();

    if (slot < 0) {
        cpu_set_errno(EMFILE);
        return -1;
    }

    int64_t fd = kobj_register(SVC_MEMFD, slot + 1);

    if (fd < 0) {
        kobj_lock_acquire();
        memfds[slot].used = false;
        kobj_lock_release();
        cpu_set_errno(EMFILE);
        return -1;
    }
    return fd;
}

static int64_t eventfd_read(int64_t id, void *buf, uint64_t count)
{
    eventfd_obj_t *e = eventfd_get(id);

    if (count < sizeof(uint64_t) || buf == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (e == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    uint64_t v;

    kobj_lock_acquire();
    if (e->counter == 0) {
        kobj_lock_release();
        cpu_set_errno(EAGAIN);
        return -1;
    }
    if (e->semaphore) {
        v = 1;
        e->counter--;
    } else {
        v = e->counter;
        e->counter = 0;
    }
    void *wk = e->wait_key;
    int32_t wev = e->wait_events;

    kobj_lock_release();

    /* A drain makes the eventfd writable and a writer may be waiting. */
    if (wk != NULL && (wev & POLLOUT))
        sched_wake_key(wk);

    if (copy_to_user(buf, &v, sizeof(v)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return (int64_t) sizeof(v);
}

static int64_t eventfd_write(int64_t id, const void *buf, uint64_t count)
{
    eventfd_obj_t *e = eventfd_get(id);
    uint64_t v;

    if (count < sizeof(uint64_t) || buf == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (e == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (copy_from_user(&v, buf, sizeof(v)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (v == ~0ULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kobj_lock_acquire();
    if (e->counter > (~0ULL - 1) - v) {
        kobj_lock_release();
        cpu_set_errno(EAGAIN);
        return -1;
    }
    e->counter += v;
    void *wk = e->wait_key;
    int32_t wev = e->wait_events;

    kobj_lock_release();

    /* A write makes the eventfd readable. */
    if (wk != NULL && (wev & POLLIN))
        sched_wake_key(wk);

    return (int64_t) sizeof(v);
}

int64_t k_epoll_create1(int32_t flags)
{
    int32_t slot = -1;

    (void) flags;
    cpu_set_errno(0);

    kobj_lock_acquire();
    for (int32_t i = 0; i < EPOLL_MAX; i++) {
        if (!epolls[i].used) {
            memset(&epolls[i], 0, sizeof(epolls[i]));
            epolls[i].used = true;
            epolls[i].refs = 1;
            slot = i;
            break;
        }
    }
    kobj_lock_release();

    if (slot < 0) {
        cpu_set_errno(EMFILE);
        return -1;
    }

    int64_t fd = kobj_register(SVC_EPOLL, slot + 1);

    if (fd < 0) {
        kobj_lock_acquire();
        epolls[slot].used = false;
        kobj_lock_release();
        cpu_set_errno(EMFILE);
        return -1;
    }
    return fd;
}

int64_t k_epoll_ctl(int32_t epfd, int32_t op, int32_t fd, void *uevent)
{
    int32_t kind = 0, svc = 0;
    int64_t sid = 0;

    cpu_set_errno(0);

    if (process_fd_get(epfd, &kind, &svc, &sid, NULL, NULL) != 0
        || svc != SVC_EPOLL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    epoll_obj_t *eo = epoll_get(sid);

    if (eo == NULL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    uint32_t events = 0;
    uint64_t data = 0;

    if (op != 2 /* EPOLL_CTL_DEL */) {
        struct {
            uint32_t events;
            uint64_t data;
        } __attribute__((packed)) ev;

        if (uevent == NULL || copy_from_user(&ev, uevent, sizeof(ev)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        events = ev.events;
        data = ev.data;
    }

    if (op == 1) {              /* EPOLL_CTL_ADD */
        int32_t slot = -1;

        for (int32_t i = 0; i < EPOLL_FD_MAX; i++) {
            if (eo->items[i].fd == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            cpu_set_errno(ENOMEM);
            return -1;
        }
        eo->items[slot].fd = fd + 1;
        eo->items[slot].events = events;
        eo->items[slot].data = data;
        eo->count++;
        return 0;
    }

    for (int32_t i = 0; i < EPOLL_FD_MAX; i++) {
        if (eo->items[i].fd == fd + 1) {
            if (op == 3) {      /* EPOLL_CTL_MOD */
                eo->items[i].events = events;
                eo->items[i].data = data;
            } else if (op == 2) {       /* EPOLL_CTL_DEL */
                eo->items[i].fd = 0;
                eo->count--;
            } else {
                cpu_set_errno(EINVAL);
                return -1;
            }
            return 0;
        }
    }

    cpu_set_errno(ENOENT);
    return -1;
}

int64_t k_epoll_wait(int32_t epfd, void *uevents, int32_t maxevents,
                     int32_t timeout)
{
    int32_t kind = 0, svc = 0;
    int64_t sid = 0;

    cpu_set_errno(0);

    if (uevents == NULL || maxevents <= 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    if (process_fd_get(epfd, &kind, &svc, &sid, NULL, NULL) != 0
        || svc != SVC_EPOLL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    epoll_obj_t *eo = epoll_get(sid);

    if (eo == NULL) {
        cpu_set_errno(EBADF);
        return -1;
    }

    int64_t start = (int64_t) (hpet_get_nanos() / 1000000);

    for (;;) {
        int32_t ready = 0;

        for (int32_t i = 0; i < EPOLL_FD_MAX && ready < maxevents; i++) {
            if (eo->items[i].fd == 0)
                continue;

            int16_t rev =
                fd_poll_revents(eo->items[i].fd - 1,
                                (int16_t) eo->items[i].events, NULL);

            if (rev) {
                struct {
                    uint32_t events;
                    uint64_t data;
                } __attribute__((packed)) ev;

                ev.events = (uint32_t) rev;
                ev.data = eo->items[i].data;
                if (copy_to_user((uint8_t *) uevents
                                 + (uint64_t) ready * sizeof(ev), &ev,
                                 sizeof(ev)) != 0) {
                    cpu_set_errno(EFAULT);
                    return -1;
                }
                ready++;
            }
        }

        if (ready > 0)
            return ready;
        if (timeout == 0)
            return 0;

        int64_t elapsed = (int64_t) (hpet_get_nanos() / 1000000) - start;

        if (timeout > 0 && elapsed >= timeout)
            return 0;
        int64_t rem = timeout > 0 ? timeout - elapsed : -1;

        process_t *t = sched_get_current_process();
        void *key = t != NULL ? (void *) &t->poll_key : NULL;

        if (key == NULL) {
            sched_sleep(rem < 0 ? 1000 : rem);
            continue;
        }

        /* Register before arming: registration blocks on IPC, whose reply
         * wait reuses the process wake key. */
        for (int32_t i = 0; i < EPOLL_FD_MAX; i++) {
            if (eo->items[i].fd != 0)
                poll_register_one(eo->items[i].fd - 1, key,
                                  (int32_t) eo->items[i].events);
        }

        /* Re-check after registering; data that arrived in the window is
         * caught here instead of being missed until the timeout. */
        bool any = false;

        for (int32_t i = 0; i < EPOLL_FD_MAX; i++) {
            if (eo->items[i].fd != 0
                && fd_poll_revents(eo->items[i].fd - 1,
                                   (int16_t) eo->items[i].events, NULL))
                any = true;
        }
        if (any)
            continue;

        sched_wait_key_begin(key);
        if (rem < 0)
            sched_wait_key_commit_infinite();
        else
            sched_wait_key_commit(rem);
    }
}

int64_t k_epoll_pwait(int32_t epfd, void *uevents, int32_t maxevents,
                      int32_t timeout, const void *sigmask)
{
    (void) sigmask;
    return k_epoll_wait(epfd, uevents, maxevents, timeout);
}

int64_t k_resolve(const char *name, uint32_t * uip)
{
    char kname[64];
    uint32_t ip = 0;

    cpu_set_errno(0);

    if (name == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (strncpy_from_user(kname, name, sizeof(kname)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    int64_t r = net_resolve(kname, &ip);

    if (r < 0) {
        cpu_set_errno((r == -1) ? ENOSYS : (int32_t) - r);
        return -1;
    }
    if (uip != NULL && copy_to_user(uip, &ip, sizeof(ip)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return 0;
}

syscall_ptr_t syscall_funcs[SYSCALL_TABLE_SIZE] = {
    [SYSCALL_READ] = (syscall_ptr_t) k_read,
    [SYSCALL_WRITE] = (syscall_ptr_t) k_write,
    [SYSCALL_OPEN] = (syscall_ptr_t) k_open,
    [SYSCALL_CLOSE] = (syscall_ptr_t) k_close,
    [SYSCALL_PREAD64] = (syscall_ptr_t) k_pread64,
    [SYSCALL_PWRITE64] = (syscall_ptr_t) k_pwrite64,
    [SYSCALL_FSTAT] = (syscall_ptr_t) k_fstat,
    [SYSCALL_LSEEK] = (syscall_ptr_t) k_seek,
    [SYSCALL_MMAP] = (syscall_ptr_t) k_vm_map,
    [SYSCALL_MPROTECT] = (syscall_ptr_t) k_mprotect,
    [SYSCALL_MUNMAP] = (syscall_ptr_t) k_vm_unmap,
    [SYSCALL_BRK] = (syscall_ptr_t) k_brk,
    [SYSCALL_RT_SIGACTION] = (syscall_ptr_t) k_sigaction,
    [SYSCALL_RT_SIGPROCMASK] = (syscall_ptr_t) k_sigprocmask,
    [SYSCALL_RT_SIGRETURN] = (syscall_ptr_t) k_sigreturn,
    [SYSCALL_KILL] = (syscall_ptr_t) k_kill,
    [SYSCALL_TKILL] = (syscall_ptr_t) k_tkill,
    [SYSCALL_TGKILL] = (syscall_ptr_t) k_tgkill,
    [SYSCALL_SET_TID_ADDRESS] = (syscall_ptr_t) k_set_tid_address,
    [SYSCALL_IOCTL] = (syscall_ptr_t) k_ioctl,
    [SYSCALL_PIPE] = (syscall_ptr_t) k_pipe,
    [SYSCALL_NANOSLEEP] = (syscall_ptr_t) k_nanosleep,
    [SYSCALL_CLOCK_NANOSLEEP] = (syscall_ptr_t) k_clock_nanosleep,
    [SYSCALL_SCHED_YIELD] = (syscall_ptr_t) k_sched_yield,
    [SYSCALL_FSYNC] = (syscall_ptr_t) k_fsync,
    [SYSCALL_TRUNCATE] = (syscall_ptr_t) k_truncate,
    [SYSCALL_FTRUNCATE] = (syscall_ptr_t) k_ftruncate,
    [SYSCALL_FDATASYNC] = (syscall_ptr_t) k_fsync,
    [SYSCALL_POLL] = (syscall_ptr_t) k_poll,
    [SYSCALL_SELECT] = (syscall_ptr_t) k_select,
    [SYSCALL_PSELECT6] = (syscall_ptr_t) k_pselect6,
    [SYSCALL_GETPID] = (syscall_ptr_t) k_getpid,
    [SYSCALL_SOCKET] = (syscall_ptr_t) k_socket,
    [SYSCALL_CONNECT] = (syscall_ptr_t) k_connect,
    [SYSCALL_ACCEPT] = (syscall_ptr_t) k_accept,
    [SYSCALL_GETSOCKNAME] = (syscall_ptr_t) k_getsockname,
    [SYSCALL_GETPEERNAME] = (syscall_ptr_t) k_getpeername,
    [SYSCALL_SHUTDOWN] = (syscall_ptr_t) k_shutdown,
    [SYSCALL_SETSOCKOPT] = (syscall_ptr_t) k_setsockopt,
    [SYSCALL_GETSOCKOPT] = (syscall_ptr_t) k_getsockopt,
    [SYSCALL_SENDTO] = (syscall_ptr_t) k_sendto,
    [SYSCALL_RECVFROM] = (syscall_ptr_t) k_recvfrom,
    [SYSCALL_BIND] = (syscall_ptr_t) k_bind,
    [SYSCALL_LISTEN] = (syscall_ptr_t) k_listen,
    [SYSCALL_CLONE] = (syscall_ptr_t) k_clone,
    [SYSCALL_FORK] = (syscall_ptr_t) k_fork,
    [SYSCALL_VFORK] = (syscall_ptr_t) k_fork,
    [SYSCALL_EXECVE] = (syscall_ptr_t) k_execve,
    [SYSCALL_EXIT] = (syscall_ptr_t) k_exit,
    [SYSCALL_EXIT_GROUP] = (syscall_ptr_t) k_exit_group,
    [SYSCALL_WAIT4] = (syscall_ptr_t) k_waitpid,
    [SYSCALL_UNAME] = (syscall_ptr_t) k_uname,
    [SYSCALL_FCNTL] = (syscall_ptr_t) k_fcntl,
    [SYSCALL_GETCWD] = (syscall_ptr_t) k_getcwd,
    [SYSCALL_CHDIR] = (syscall_ptr_t) k_chdir,
    [SYSCALL_FCHDIR] = (syscall_ptr_t) k_fchdir,
    [SYSCALL_UNLINK] = (syscall_ptr_t) k_unlink,
    [SYSCALL_RMDIR] = (syscall_ptr_t) k_rmdir,
    [SYSCALL_MKDIRAT] = (syscall_ptr_t) k_mkdirat,
    [SYSCALL_SYMLINKAT] = (syscall_ptr_t) k_symlinkat,
    [SYSCALL_LINKAT] = (syscall_ptr_t) k_linkat,
    [SYSCALL_MKNODAT] = (syscall_ptr_t) k_mknodat,
    [SYSCALL_RENAMEAT] = (syscall_ptr_t) k_renameat,
    [SYSCALL_RENAMEAT2] = (syscall_ptr_t) k_renameat,
    [SYSCALL_READLINK] = (syscall_ptr_t) k_readlink,
    [SYSCALL_GETRUSAGE] = (syscall_ptr_t) k_getrusage,
    [SYSCALL_SETPGID] = (syscall_ptr_t) k_setpgid,
    [SYSCALL_GETPPID] = (syscall_ptr_t) k_getppid,
    [SYSCALL_GETPGRP] = (syscall_ptr_t) k_getpgrp,
    [SYSCALL_SETSID] = (syscall_ptr_t) k_setsid,
    [SYSCALL_GETPGID] = (syscall_ptr_t) k_getpgid,
    [SYSCALL_GETSID] = (syscall_ptr_t) k_getsid,
    [SYSCALL_STATFS] = (syscall_ptr_t) k_statfs,
    [SYSCALL_FSTATFS] = (syscall_ptr_t) k_fstatfs,
    [SYSCALL_GETDENTS64] = (syscall_ptr_t) k_readdir,
    [SYSCALL_CLOCK_GETTIME] = (syscall_ptr_t) k_getclock,
    [SYSCALL_OPENAT] = (syscall_ptr_t) k_openat,
    [SYSCALL_NEWFSTATAT] = (syscall_ptr_t) k_fstatat,
    [SYSCALL_FACCESSAT] = (syscall_ptr_t) k_faccessat,
    [SYSCALL_ACCESS] = (syscall_ptr_t) k_access,
    [SYSCALL_DUP3] = (syscall_ptr_t) k_dup3,
    [SYSCALL_PRLIMIT64] = (syscall_ptr_t) k_prlimit64,
    [SYSCALL_EVENTFD2] = (syscall_ptr_t) k_eventfd2,
    [SYSCALL_MEMFD_CREATE] = (syscall_ptr_t) k_memfd_create,
    [SYSCALL_EPOLL_CREATE1] = (syscall_ptr_t) k_epoll_create1,
    [SYSCALL_EPOLL_CTL] = (syscall_ptr_t) k_epoll_ctl,
    [SYSCALL_EPOLL_WAIT] = (syscall_ptr_t) k_epoll_wait,
    [SYSCALL_SIGNALFD4] = (syscall_ptr_t) k_signalfd4,
    [SYSCALL_EPOLL_PWAIT] = (syscall_ptr_t) k_epoll_pwait,
    [SYSCALL_RESOLVE] = (syscall_ptr_t) k_resolve,
    [SYSCALL_POLL_WAKE] = (syscall_ptr_t) k_poll_wake,
    [SYSCALL_GETRANDOM] = (syscall_ptr_t) k_getentropy,
    [SYSCALL_DEBUGLOG] = (syscall_ptr_t) k_debug_log,
    [SYSCALL_SET_FS_BASE] = (syscall_ptr_t) k_set_fs_base,
    [SYSCALL_MEMINFO] = (syscall_ptr_t) k_meminfo,
    [SYSCALL_MEM_STATS] = (syscall_ptr_t) k_mem_stats,
    [SYSCALL_RUNCMD] = (syscall_ptr_t) k_runcmd,
    [SYSCALL_CHMOD] = (syscall_ptr_t) k_chmod,
    [SYSCALL_CHOWN] = (syscall_ptr_t) k_chown,
    [SYSCALL_LCHOWN] = (syscall_ptr_t) k_chown,
    [SYSCALL_FCHOWNAT] = (syscall_ptr_t) k_fchownat,
    [SYSCALL_GETUID] = (syscall_ptr_t) k_getuid,
    [SYSCALL_GETGID] = (syscall_ptr_t) k_getgid,
    [SYSCALL_GETEUID] = (syscall_ptr_t) k_geteuid,
    [SYSCALL_GETEGID] = (syscall_ptr_t) k_getegid,
    [SYSCALL_SETUID] = (syscall_ptr_t) k_setuid,
    [SYSCALL_SETGID] = (syscall_ptr_t) k_setgid,
    [SYSCALL_UMASK] = (syscall_ptr_t) k_umask,
    [SYSCALL_FUTEX_WAIT] = (syscall_ptr_t) k_futex_wait,
    [SYSCALL_FUTEX_WAKE] = (syscall_ptr_t) k_futex_wake,
    [SYSCALL_EP_CREATE] = (syscall_ptr_t) k_ep_create,
    [SYSCALL_IPC_SEND] = (syscall_ptr_t) k_ipc_send,
    [SYSCALL_IPC_RECV] = (syscall_ptr_t) k_ipc_recv,
    [SYSCALL_IPC_CALL] = (syscall_ptr_t) k_ipc_call,
    [SYSCALL_IPC_REPLY] = (syscall_ptr_t) k_ipc_reply,
    [SYSCALL_MEM_ALLOC] = (syscall_ptr_t) k_mem_alloc,
    [SYSCALL_MEM_MAP] = (syscall_ptr_t) k_mem_map,
    [SYSCALL_IRQ_BIND] = (syscall_ptr_t) k_irq_bind,
    [SYSCALL_IRQ_ACK] = (syscall_ptr_t) k_irq_ack,
    [SYSCALL_HANDLE_CLOSE] = (syscall_ptr_t) k_handle_close,
    [SYSCALL_IOPORT_ACCESS] = (syscall_ptr_t) k_ioport_access,
    [SYSCALL_BOOTINFO] = (syscall_ptr_t) k_bootinfo,
    [SYSCALL_IPC_RECV_NB] = (syscall_ptr_t) k_ipc_recv_nb,
    [SYSCALL_IPC_RECV_TIMEOUT] = (syscall_ptr_t) k_ipc_recv_timeout,
    [SYSCALL_MEM_UNMAP] = (syscall_ptr_t) k_mem_unmap,
    [SYSCALL_HANDLE_DUP] = (syscall_ptr_t) k_handle_dup,
    [SYSCALL_SERIAL_WRITE] = (syscall_ptr_t) k_serial_write,
    [SYSCALL_PROC_SPAWN] = (syscall_ptr_t) k_proc_spawn,
    [SYSCALL_PROC_MAP] = (syscall_ptr_t) k_proc_map,
    [SYSCALL_PROC_SET_ENTRY] = (syscall_ptr_t) k_proc_set_entry,
    [SYSCALL_PROC_START] = (syscall_ptr_t) k_proc_start,
    [SYSCALL_SOCKET_CLOSE] = (syscall_ptr_t) k_socket_close,
    [SYSCALL_KOBJ_REF] = (syscall_ptr_t) k_kobj_ref,
    [SYSCALL_KOBJ_UNREF] = (syscall_ptr_t) k_kobj_unref,
    [SYSCALL_BIND_UNIX] = (syscall_ptr_t) k_bind_unix,
    [SYSCALL_CONNECT_UNIX] = (syscall_ptr_t) k_connect_unix,
    [SYSCALL_SENDMSG] = (syscall_ptr_t) k_sendmsg,
    [SYSCALL_RECVMSG] = (syscall_ptr_t) k_recvmsg,
    [SYSCALL_MEM_PHYS] = (syscall_ptr_t) k_mem_phys,
    [SYSCALL_USB_BLOCK_INFO] = (syscall_ptr_t) k_usb_block_info,
    [SYSCALL_USB_BLOCK_READ] = (syscall_ptr_t) k_usb_block_read,
    [SYSCALL_USB_BLOCK_WRITE] = (syscall_ptr_t) k_usb_block_write,
    [SYSCALL_KLOG] = (syscall_ptr_t) k_klog
};

void syscall_init(void)
{
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);        /* Enable syscall */

    uint64_t star = (uint64_t) DEFAULT_KMODE_CODE << 32;
    star |= (uint64_t) (DEFAULT_KMODE_DATA | 3) << 48;

    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t) & syscall_handler);
    write_msr(MSR_SFMASK, X86_EFLAGS_TF | X86_EFLAGS_DF | X86_EFLAGS_IF
              | X86_EFLAGS_IOPL | X86_EFLAGS_AC | X86_EFLAGS_NT);

    klogi("SYSCALL: MSR_EFER=0x%016lx MSR_STAR=0x%016lx MSR_LSTAR=0x%016lx\n",
          read_msr(MSR_EFER), read_msr(MSR_STAR), read_msr(MSR_LSTAR));
}
