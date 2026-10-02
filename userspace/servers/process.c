/**-----------------------------------------------------------------------------

 @file    process.c
 @brief   Userspace process server

 @details
 @verbatim

   Owns the per-process file-descriptor table. The kernel opens, closes,
   duplicates, forks, execs and exits through it, and resolves a fd with
   PROC_FD_GET before doing I/O. Descriptors backed by a userspace server store
   the service and the server-side fd; descriptors backed by the in-kernel
   filesystem are marked PROC_FD_KERNEL and their content stays in the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

#define PROC_PID_MAX    128
#define PROC_FD_MAX     32
#define PROC_FD_BASE    3       /* fds 0..2 are stdin/stdout/stderr */

/* Service ids sent by the kernel; must match kernel/router/router.h. */
#define SVC_FS_ID       1
#define SVC_PIPE_ID     5
#define SVC_TTY_ID      6
#define SVC_FAT_ID      7

/**
 * @brief One descriptor in a process's file-descriptor table
 */
typedef struct {
    bool used;
    int32_t kind;
    int32_t svc;
    int64_t server_fd;
    uint64_t size;
    uint64_t seek_pos;
    int64_t mode;
    bool cloexec;
} proc_fd_t;

static proc_fd_t table[PROC_PID_MAX][PROC_FD_MAX];

/* Parent/child bookkeeping for wait/exit. The kernel reports fork and exit
 * events; the server decides when a child is reapable. A process counts as
 * exited only after it exits and all of its children are gone, so a parent that
 * exec'd (spawn then exit) stays waitable until its replacement finishes. */
typedef struct {
    bool used;
    int32_t ppid;
    bool exited;
    int32_t status;
} proc_rec_t;

static proc_rec_t procs[PROC_PID_MAX];

static bootinfo_t bi;

/* Service endpoints the process server notifies on fork. */
static uint64_t fs_ep;
static uint64_t pipe_ep;
static uint64_t tty_ep;
static uint64_t fat_ep;

static uint64_t ep_for_svc(int32_t svc)
{
    if (svc == SVC_FS_ID)
        return fs_ep;
    if (svc == SVC_PIPE_ID)
        return pipe_ep;
    if (svc == SVC_TTY_ID)
        return tty_ep;
    if (svc == SVC_FAT_ID)
        return fat_ep;
    return 0;
}

static uint64_t close_tag_for_svc(int32_t svc)
{
    if (svc == SVC_FS_ID)
        return VFS_CLOSE;
    if (svc == SVC_PIPE_ID)
        return PIPE_CLOSE;
    if (svc == SVC_FAT_ID)
        return FAT_CLOSE;
    return 0;
}

static int32_t fd_alloc(int32_t pid, proc_fd_t **out)
{
    if (pid < 0 || pid >= PROC_PID_MAX)
        return -1;

    for (int32_t fd = PROC_FD_BASE; fd < PROC_FD_MAX; fd++) {
        if (!table[pid][fd].used) {
            *out = &table[pid][fd];
            return fd;
        }
    }

    return -1;
}

static proc_fd_t *fd_get(int32_t pid, int32_t fd)
{
    if (pid < 0 || pid >= PROC_PID_MAX || fd < 0 || fd >= PROC_FD_MAX)
        return NULL;

    return table[pid][fd].used ? &table[pid][fd] : NULL;
}

static void reply_fd(proc_fd_t * e, sys_ipc_msg_t * rep)
{
    rep->words[1] = (uint64_t) (int64_t) e->kind;
    rep->words[2] = (uint64_t) (int64_t) e->svc;
    rep->words[3] = (uint64_t) e->server_fd;
    rep->words[4] = e->size;
    rep->words[5] = e->seek_pos;
}

/* --- wait/exit bookkeeping ------------------------------------------------ */

static void proc_register(int32_t pid, int32_t ppid)
{
    if (pid < 0 || pid >= PROC_PID_MAX)
        return;

    procs[pid].used = true;
    procs[pid].ppid = ppid;
    procs[pid].exited = false;
    procs[pid].status = 0;
}

static void proc_mark_exit(int32_t pid, int32_t status)
{
    if (pid < 0 || pid >= PROC_PID_MAX)
        return;

    procs[pid].used = true;
    procs[pid].exited = true;
    procs[pid].status = status;
}

static bool proc_all_children_done(int32_t pid)
{
    for (int32_t i = 0; i < PROC_PID_MAX; i++) {
        if (procs[i].used && procs[i].ppid == pid && !procs[i].exited)
            return false;
    }
    return true;
}

/* True when pid has exited and every child has exited too. An unknown pid
 * counts as done. */
static bool proc_done(int32_t pid)
{
    if (pid < 0 || pid >= PROC_PID_MAX || !procs[pid].used)
        return true;
    if (!procs[pid].exited)
        return false;
    return proc_all_children_done(pid);
}

static void clone_fds(int32_t parent, int32_t child)
{
    if (parent < 0 || parent >= PROC_PID_MAX || child < 0
        || child >= PROC_PID_MAX)
        return;

    memcpy(table[child], table[parent], sizeof(table[child]));

    /* The child inherits a reference to each server-side open file
     * description, so tell the owning server to add one. */
    for (int32_t fd = 0; fd < PROC_FD_MAX; fd++) {
        proc_fd_t *e = &table[child][fd];
        uint64_t ep;

        if (!e->used || e->kind != PROC_FD_SERVER)
            continue;

        ep = ep_for_svc(e->svc);
        if (ep == 0)
            continue;

        sys_ipc_msg_t fm;
        memset(&fm, 0, sizeof(fm));
        fm.tag = VFS_FD_FORK;
        fm.words[0] = (uint64_t) e->server_fd;
        sys_ipc_send((int64_t) ep, &fm);
    }
}

/* Close and clear every descriptor of pid marked close-on-exec. Runs after the
 * fd table has been cloned for an exec, so the child does not inherit them. */
static void close_cloexec(int32_t pid)
{
    if (pid < 0 || pid >= PROC_PID_MAX)
        return;

    for (int32_t fd = PROC_FD_BASE; fd < PROC_FD_MAX; fd++) {
        proc_fd_t *e = &table[pid][fd];

        if (!e->used || !e->cloexec)
            continue;

        if (e->kind == PROC_FD_SERVER) {
            uint64_t ep = ep_for_svc(e->svc);
            uint64_t tag = close_tag_for_svc(e->svc);

            if (ep != 0 && tag != 0) {
                sys_ipc_msg_t cm;
                memset(&cm, 0, sizeof(cm));
                cm.tag = tag;
                cm.words[0] = (uint64_t) e->server_fd;
                sys_ipc_send((int64_t) ep, &cm);
            }
        }
        e->used = false;
    }
}

/* --- exec: parse an ELF image and load it into a fresh process ------------ */

#define PROC_EXEC_ELF_ADDR   0x40000000UL
#define PROC_EXEC_STR_ADDR   0x41000000UL
#define PROC_EXEC_SEG_ADDR   0x42000000UL
#define PROC_EXEC_STK_ADDR   0x0000200000000000UL
#define PROC_EXEC_STK_SIZE   0x20000UL
#define PROC_EXEC_ARG_MAX    64
#define EXEC_PROT_READ       0x1
#define EXEC_PROT_WRITE      0x2

typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} elf64_hdr_t;

typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} elf64_phdr_t;

#define EXEC_PT_LOAD    1
#define EXEC_PT_PHDR    6
#define EXEC_PF_W       2

static uint64_t build_stack(uint8_t * base, uint64_t size, uint64_t argc,
                            char **argv, uint64_t envc, char **envp,
                            uint64_t entry, uint64_t phdr, uint64_t phentsize,
                            uint64_t phnum)
{
    uint64_t sp = size;
    uint64_t argp[PROC_EXEC_ARG_MAX], envpp[PROC_EXEC_ARG_MAX];

    for (uint64_t i = 0; i < envc; i++) {
        uint64_t n = strlen(envp[i]) + 1;
        sp -= n;
        memcpy(base + sp, envp[i], n);
        envpp[i] = PROC_EXEC_STK_ADDR + sp;
    }
    for (uint64_t i = 0; i < argc; i++) {
        uint64_t n = strlen(argv[i]) + 1;
        sp -= n;
        memcpy(base + sp, argv[i], n);
        argp[i] = PROC_EXEC_STK_ADDR + sp;
    }

    sp &= ~(uint64_t) 0xf;

#define SPUSH(v) do { sp -= 8; *(uint64_t *)(base + sp) = (uint64_t) (v); } while (0)
    SPUSH(0);
    SPUSH(0);                   /* AT_NULL */
    SPUSH(entry);
    SPUSH(10);                  /* AT_ENTRY */
    SPUSH(phdr);
    SPUSH(20);                  /* AT_PHDR */
    SPUSH(phentsize);
    SPUSH(21);                  /* AT_PHENT */
    SPUSH(phnum);
    SPUSH(22);                  /* AT_PHNUM */
    SPUSH(0);                   /* end of environment */
    for (uint64_t i = envc; i > 0; i--)
        SPUSH(envpp[i - 1]);
    SPUSH(0);                   /* end of arguments */
    for (uint64_t i = argc; i > 0; i--)
        SPUSH(argp[i - 1]);
    SPUSH(argc);
#undef SPUSH

    return sp;
}

/* Load into a fresh child of caller the ELF packed in the buffer passed as
 * xfer[1], along with its argv/envp. Returns the child pid or -1. */
static int32_t exec_load(int64_t caller, int64_t bufh)
{
    uint8_t *buf = (uint8_t *) PROC_EXEC_ELF_ADDR;
    char *argv[PROC_EXEC_ARG_MAX], *envp[PROC_EXEC_ARG_MAX];
    char name[64];
    uint64_t argc, envc, phdr_vaddr = 0;
    int32_t pid = -1;

    if (sys_mem_map(bufh, PROC_EXEC_ELF_ADDR, EXEC_PROT_READ) != 0)
        return -1;

    uint64_t *h = (uint64_t *) buf;
    uint8_t *elf = buf + h[6];

    argc = h[4];
    envc = h[5];
    if (argc > PROC_EXEC_ARG_MAX)
        argc = PROC_EXEC_ARG_MAX;
    if (envc > PROC_EXEC_ARG_MAX)
        envc = PROC_EXEC_ARG_MAX;
    for (uint64_t i = 0; i < argc; i++)
        argv[i] = (char *)(buf + ((uint64_t *) (buf + h[2]))[i]);
    for (uint64_t i = 0; i < envc; i++)
        envp[i] = (char *)(buf + ((uint64_t *) (buf + h[3]))[i]);

    elf64_hdr_t *eh = (elf64_hdr_t *) elf;

    if (eh->ident[0] != 0x7f || eh->ident[1] != 'E' || eh->ident[2] != 'L'
        || eh->ident[3] != 'F' || eh->phentsize != sizeof(elf64_phdr_t))
        goto out;

    {
        const char *path = (char *)(buf + h[0]);
        uint64_t n = strlen(path);
        uint64_t start = 0;

        for (uint64_t k = 0; k < n; k++)
            if (path[k] == '/')
                start = k + 1;

        uint64_t m = n - start;

        if (m > sizeof(name) - 1)
            m = sizeof(name) - 1;
        memcpy(name, path + start, m);
        name[m] = '\0';
    }

    pid = sys_proc_spawn(caller, name);
    if (pid < 0)
        goto out;
    proc_register(pid, (int32_t) caller);

    for (uint64_t i = 0; i < eh->phnum; i++) {
        elf64_phdr_t *ph =
            (elf64_phdr_t *) (elf + eh->phoff + i * eh->phentsize);

        if (ph->type == EXEC_PT_PHDR) {
            phdr_vaddr = ph->vaddr;
            continue;
        }
        if (ph->type != EXEC_PT_LOAD || ph->memsz == 0)
            continue;

        uint64_t misalign = ph->vaddr & 0xfff;
        uint64_t vaddr = ph->vaddr & ~(uint64_t) 0xfff;
        uint64_t np = (misalign + ph->memsz + 0xfff) / 0x1000;
        int64_t mh = sys_mem_alloc(np * 0x1000);

        if (mh < 0)
            goto out;
        if (sys_mem_map(mh, PROC_EXEC_SEG_ADDR, EXEC_PROT_WRITE) != 0) {
            sys_handle_close(mh);
            goto out;
        }
        memcpy((uint8_t *) PROC_EXEC_SEG_ADDR + misalign, elf + ph->offset,
               ph->filesz);
        sys_mem_unmap(mh, PROC_EXEC_SEG_ADDR);

        int32_t prot = (ph->flags & EXEC_PF_W)
            ? (EXEC_PROT_READ | EXEC_PROT_WRITE) : EXEC_PROT_READ;

        if (sys_proc_map(pid, vaddr, (int32_t) mh, prot) != 0) {
            sys_handle_close(mh);
            goto out;
        }
        sys_handle_close(mh);
    }

    {
        int64_t mh = sys_mem_alloc(PROC_EXEC_STK_SIZE);

        if (mh < 0)
            goto out;
        if (sys_mem_map(mh, PROC_EXEC_SEG_ADDR, EXEC_PROT_WRITE) != 0) {
            sys_handle_close(mh);
            goto out;
        }
        uint64_t rsp = build_stack((uint8_t *) PROC_EXEC_SEG_ADDR,
                                   PROC_EXEC_STK_SIZE, argc, argv, envc, envp,
                                   eh->entry, phdr_vaddr, eh->phentsize,
                                   eh->phnum);
        sys_mem_unmap(mh, PROC_EXEC_SEG_ADDR);

        if (sys_proc_map(pid, PROC_EXEC_STK_ADDR, (int32_t) mh,
                         EXEC_PROT_READ | EXEC_PROT_WRITE) != 0) {
            sys_handle_close(mh);
            goto out;
        }
        sys_proc_set_entry(pid, eh->entry, PROC_EXEC_STK_ADDR + rsp);
        sys_handle_close(mh);
    }

    clone_fds(caller, pid);
    close_cloexec(pid);
    sys_proc_start(pid);

  out:
    sys_mem_unmap(bufh, PROC_EXEC_ELF_ADDR);
    return pid;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    int32_t pid = (int32_t) m->words[0];
    int32_t fd = (int32_t) m->words[1];

    switch (m->tag) {
    case PROC_PING:
        rep->words[0] = PROC_PONG;
        return;

    case PROC_FD_OPEN:{
            proc_fd_t *e = NULL;
            int32_t nfd = fd_alloc(pid, &e);

            if (nfd < 0) {
                rep->words[0] = (uint64_t) (int64_t) -24;       /* -EMFILE */
                return;
            }

            memset(e, 0, sizeof(*e));
            e->used = true;
            e->svc = (int32_t) m->words[1];
            e->server_fd = (int64_t) m->words[2];
            e->size = m->words[3];
            e->mode = (int64_t) m->words[4];
            e->kind = PROC_FD_SERVER;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) nfd;
            return;
        }

    case PROC_FD_GET:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;        /* -EBADF */
                return;
            }
            rep->words[0] = 0;
            reply_fd(e, rep);
            return;
        }

    case PROC_FD_CLOSE:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }
            rep->words[0] = 0;
            reply_fd(e, rep);
            e->used = false;
            return;
        }

    case PROC_FD_DUP:{
            proc_fd_t *src = fd_get(pid, fd);
            int32_t newfd = (int32_t) m->words[2];
            proc_fd_t *dst = NULL;

            if (src == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }

            if (newfd < 0) {
                newfd = fd_alloc(pid, &dst);
                if (newfd < 0) {
                    rep->words[0] = (uint64_t) (int64_t) -24;
                    return;
                }
            } else if (newfd < PROC_FD_MAX) {
                dst = &table[pid][newfd];
            } else {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }

            *dst = *src;
            dst->used = true;
            dst->cloexec = false;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) newfd;
            return;
        }

    case PROC_FD_FCNTL:{
            proc_fd_t *e = fd_get(pid, fd);
            int32_t cmd = (int32_t) m->words[2];
            int32_t arg = (int32_t) m->words[3];

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }
            if (cmd == F_GETFD) {
                rep->words[1] = e->cloexec ? FD_CLOEXEC : 0;
            } else if (cmd == F_SETFD) {
                e->cloexec = (arg & FD_CLOEXEC) != 0;
                rep->words[1] = e->cloexec ? FD_CLOEXEC : 0;
            } else {
                rep->words[0] = (uint64_t) (int64_t) -22;       /* -EINVAL */
                return;
            }
            rep->words[0] = 0;
            return;
        }

    case PROC_FD_SEEK:{
            proc_fd_t *e = fd_get(pid, fd);

            if (e == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                return;
            }
            /* The offset is only tracked here for path-keyed (FAT) files; the
             * VFS/pipe servers advance their own offset. */
            if ((int64_t) m->words[2] == 0)          /* SEEK_SET */
                e->seek_pos = m->words[1];
            else if ((int64_t) m->words[2] == 1)     /* SEEK_CUR */
                e->seek_pos += m->words[1];
            else
                e->seek_pos = e->size + m->words[1];  /* SEEK_END */
            rep->words[0] = 0;
            rep->words[1] = e->seek_pos;
            return;
        }

    case PROC_FD_FORK:{
            int32_t child = (int32_t) m->words[1];

            if (pid < 0 || pid >= PROC_PID_MAX || child < 0
                || child >= PROC_PID_MAX) {
                rep->words[0] = (uint64_t) (int64_t) -22;
                return;
            }
            clone_fds(pid, child);
            proc_register(child, pid);
            rep->words[0] = 0;
            return;
        }

    case PROC_EXIT:
        proc_mark_exit(pid, (int32_t) m->words[1]);
        rep->words[0] = 0;
        return;

    case PROC_WAIT:{
            int32_t parent = pid;
            int32_t target = (int32_t) m->words[1];
            bool wait_any = (target == -1 || target == 0);
            bool nohang = (m->words[2] != 0);
            bool have = false;

            for (int32_t i = 1; i < PROC_PID_MAX; i++) {
                if (!procs[i].used || procs[i].ppid != parent)
                    continue;
                if (!wait_any && i != target)
                    continue;

                have = true;
                if (proc_done(i)) {
                    rep->words[0] = (uint64_t) i;
                    rep->words[1] = (uint64_t) (int64_t) procs[i].status;
                    procs[i].used = false;
                    return;
                }
            }

            if (!have) {
                rep->words[0] = (uint64_t) (int64_t) -1;        /* -ECHILD */
                return;
            }
            rep->words[0] = nohang ? 0 : (uint64_t) (int64_t) PROC_WAIT_BLOCK;
            return;
        }

    case PROC_EXEC:{
            int32_t child = exec_load((int64_t) pid, (int64_t) m->xfer[1]);

            sys_handle_close((int64_t) m->xfer[1]);
            if (child < 0) {
                rep->words[0] = (uint64_t) (int64_t) -8;        /* -ENOEXEC */
                return;
            }
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) child;
            return;
        }

    case PROC_FD_EXIT:
        if (pid >= 0 && pid < PROC_PID_MAX) {
            for (int32_t i = 0; i < PROC_FD_MAX; i++) {
                proc_fd_t *e = &table[pid][i];

                if (!e->used || e->kind != PROC_FD_SERVER)
                    continue;
                uint64_t ep = ep_for_svc(e->svc);
                uint64_t tag = close_tag_for_svc(e->svc);

                if (ep != 0 && tag != 0) {
                    sys_ipc_msg_t cm;
                    memset(&cm, 0, sizeof(cm));
                    cm.tag = tag;
                    cm.words[0] = (uint64_t) e->server_fd;
                    sys_ipc_send((int64_t) ep, &cm);
                }
            }
            memset(table[pid], 0, sizeof(table[pid]));
        }
        rep->words[0] = 0;
        return;

    default:
        break;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int32_t main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    fs_ep = bi.fs_ep;
    pipe_ep = bi.pipe_ep;
    tty_ep = bi.tty_ep;
    fat_ep = bi.fat_ep;

    for (;;) {
        sys_ipc_msg_t m;
        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 500) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;
        handle(&m, &rep);

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
