/**-----------------------------------------------------------------------------

 @file    selftest.c
 @brief   Boot-time self-test for the IPC/handle layer
 @details
 @verbatim

   Runs in a kernel process once the scheduler is up. It exercises the endpoint
   queue, the IPC timeout path, and the per-process handle table, then logs
   "MK: selftest PASS" or "MK: selftest FAIL".

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>

#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <ipc/selftest.h>
#include <mm/memobj.h>
#include <mm/mm.h>
#include <proc/sched.h>
#include <proc/process.h>
#include <proc/syscall.h>

/* A minimal address space (PML4 only) used to check memory-object mapping
 * without disturbing the kernel address space. The page tables created while
 * mapping are tracked in mem_list and released again by the free helper. */
static addrspace_t *test_addrspace(void)
{
    addrspace_t *as = kmalloc(sizeof(addrspace_t));

    if (as == NULL)
        return NULL;

    memset(as, 0, sizeof(*as));
    as->PML4 = kmalloc_chunk(PAGE_SIZE * 8, __func__, __LINE__);
    if (as->PML4 == NULL) {
        kmfree(as);
        return NULL;
    }

    memset(as->PML4, 0, PAGE_SIZE * 8);
    spinlock_init(&as->lock);
    as->initialized = true;
    return as;
}

static void test_addrspace_free(addrspace_t * as)
{
    uint64_t n = vec_length(&as->mem_list);

    for (uint64_t i = 0; i < n; i++)
        pmm_free(vec_at(&as->mem_list, i), 8, __func__, __LINE__);
    vec_erase_all(&as->mem_list);

    kmfree_chunk((void *) as->PML4, __func__, __LINE__);
    kmfree(as);
}

_Noreturn void mk_selftest_process(pid_t pid)
{
    (void) pid;

    bool ok = true;
    endpoint_t *ep = endpoint_create();

    if (ep == NULL) {
        ok = false;
    } else {
        ipc_msg_t m;
        memset(&m, 0, sizeof(m));
        m.tag = 0x1234;
        m.words[0] = 0xABCDEF;

        if (ipc_send(ep, &m) != 0)
            ok = false;

        ipc_msg_t r;
        if (ipc_recv_timeout(ep, &r, 100) != 0 || r.tag != 0x1234
            || r.words[0] != 0xABCDEF)
            ok = false;

        /* An empty endpoint must time out instead of blocking forever. */
        ipc_msg_t r2;
        if (ipc_recv_timeout(ep, &r2, 10) == 0)
            ok = false;

        /* Handle table round trip. */
        process_t *t = sched_get_current_process();
        handle_t h = handle_alloc(&t->handles, endpoint_object(ep),
                                  HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV);
        if (h == HANDLE_INVALID) {
            ok = false;
        } else {
            kernel_object_t *o = handle_get(&t->handles, h, HANDLE_RIGHT_SEND);
            if (o == NULL || o->type != OBJ_ENDPOINT)
                ok = false;
            if (handle_close(&t->handles, h) != 0)
                ok = false;
        }

        object_unref(endpoint_object(ep));
    }

    /* IRQ object delivery to a bound endpoint. Line 20 is outside the
     * hardware range so this cannot interfere with a real driver. */
    if (ok) {
        irq_obj_t *io = irq_create(20);
        endpoint_t *iep = endpoint_create();
        if (io == NULL || iep == NULL) {
            ok = false;
        } else {
            irq_bind(io, iep);
            if (!irq_deliver(20))
                ok = false;

            ipc_msg_t im;
            if (ipc_recv_timeout(iep, &im, 100) != 0
                || im.tag != IRQ_NOTIFY_TAG || im.words[0] != 20)
                ok = false;

            object_unref(endpoint_object(iep));
            object_unref(irq_object(io));
        }
    }

    /* Memory objects: one object mapped into two address spaces, sharing the
     * same physical pages. */
    if (ok) {
        const uint64_t va = 0x40000000;
        const uint64_t bytes = 2 * PAGE_SIZE;
        memobj_t *m = memobj_create(bytes);
        addrspace_t *a = test_addrspace();
        addrspace_t *b = test_addrspace();

        if (m == NULL || a == NULL || b == NULL) {
            ok = false;
        } else {
            uint64_t p0 = memobj_page(m, 0);
            uint64_t p1 = memobj_page(m, 1);

            if (p0 == 0 || p1 == 0 || p0 == p1)
                ok = false;

            if (memobj_map(m, a, va, PROT_READ | PROT_WRITE) != 0
                || memobj_map(m, b, va, PROT_READ | PROT_WRITE) != 0)
                ok = false;

            if (vmm_get_paddr(a, va) != p0
                || vmm_get_paddr(a, va + PAGE_SIZE) != p1
                || vmm_get_paddr(b, va) != p0
                || vmm_get_paddr(b, va + PAGE_SIZE) != p1)
                ok = false;

            /* Exchange data: write through one mapping, read the other. */
            *(uint64_t *) PHYS_TO_VIRT(vmm_get_paddr(a, va)) =
                0x1122334455667788ULL;
            if (*(uint64_t *) PHYS_TO_VIRT(vmm_get_paddr(b, va))
                != 0x1122334455667788ULL)
                ok = false;

            memobj_unmap(m, a, va);
            memobj_unmap(m, b, va);
        }

        if (a != NULL)
            test_addrspace_free(a);
        if (b != NULL)
            test_addrspace_free(b);
        if (m != NULL)
            memobj_unref(m);
    }

    /* Handle transfer: an object staged on a message is handed to the
     * receiver with the same identity. */
    if (ok) {
        endpoint_t *tep = endpoint_create();
        memobj_t *mo = memobj_create(PAGE_SIZE);

        if (tep == NULL || mo == NULL) {
            ok = false;
        } else {
            kernel_object_t *send_obj = memobj_object(mo);
            uint32_t sright = HANDLE_RIGHT_READ | HANDLE_RIGHT_MAP;
            ipc_msg_t m;

            object_ref(send_obj);       /* the reference being moved */
            memset(&m, 0, sizeof(m));
            m.tag = 0xBEEF;

            if (ipc_send_objs(tep, &m, &send_obj, &sright, 1) != 0)
                ok = false;

            ipc_msg_t r;
            kernel_object_t *got[2];
            uint32_t gright[2] = { 0, 0 };
            uint8_t gn = 0;

            if (ipc_recv_timeout_objs(tep, &r, got, gright, &gn, 100) != 0
                || gn != 1 || got[0] != send_obj || gright[0] != sright
                || r.tag != 0xBEEF)
                ok = false;
            if (gn == 1)
                object_unref(got[0]);   /* the receiver's reference */

            object_unref(endpoint_object(tep));
        }
        if (mo != NULL)
            object_unref(memobj_object(mo));
    }

    klogi("MK: selftest %s\n", ok ? "PASS" : "FAIL");

    for (;;)
        asm volatile ("hlt");
}
