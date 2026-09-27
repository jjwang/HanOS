/**-----------------------------------------------------------------------------

 @file    ipc_buf.c
 @brief   Implementation of bulk-transfer helpers

 **-----------------------------------------------------------------------------
 */
#include <string.h>

#include <lib/klog.h>
#include <mm/ipc_buf.h>
#include <mm/memobj.h>
#include <mm/uaccess.h>
#include <proc/sched.h>

static uint64_t chunk_len(uint64_t offset, uint64_t len)
{
    uint64_t in_page = PAGE_SIZE - (offset & (PAGE_SIZE - 1));

    return in_page < len ? in_page : len;
}

/* Copy from process t's user buffer into the object, page by page. */
static bool copy_in(memobj_t * m, process_t * t, const void *uptr, uint64_t len)
{
    bool current = (t == sched_get_current_process());
    uint64_t done = 0;

    while (done < len) {
        uint64_t chunk = chunk_len(done, len);
        void *kdst = (void *) (PHYS_TO_VIRT(memobj_page(m, done / PAGE_SIZE))
                               + (done & (PAGE_SIZE - 1)));
        uint64_t left;

        if (current)
            left = copy_from_user(kdst, (const uint8_t *) uptr + done, chunk);
        else
            left = copy_from_process(t, kdst, (const uint8_t *) uptr + done,
                                     chunk);

        if (left != 0)
            return false;

        done += chunk;
    }

    return true;
}

/* Copy from the object into process t's user buffer, page by page. */
static bool copy_out(memobj_t * m, process_t * t, void *uptr, uint64_t len)
{
    bool current = (t == sched_get_current_process());
    uint64_t done = 0;

    while (done < len) {
        uint64_t chunk = chunk_len(done, len);
        const void *ksrc = (const void *) (PHYS_TO_VIRT(memobj_page
                                                        (m, done / PAGE_SIZE))
                                           + (done & (PAGE_SIZE - 1)));
        uint64_t left;

        if (current)
            left = copy_to_user((uint8_t *) uptr + done, ksrc, chunk);
        else
            left = copy_to_process(t, (uint8_t *) uptr + done, ksrc, chunk);

        if (left != 0)
            return false;

        done += chunk;
    }

    return true;
}

static handle_t publish(memobj_t * m)
{
    process_t *cur = sched_get_current_process();

    if (cur == NULL)
        return HANDLE_INVALID;

    return handle_alloc(&cur->handles, memobj_object(m),
                        HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                        | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
}

int ipc_buf_from_user(process_t * t, const void *uptr, uint64_t len,
                      handle_t * out)
{
    if (t == NULL || uptr == NULL || len == 0 || out == NULL)
        return -1;

    memobj_t *m = memobj_create(len);
    if (m == NULL)
        return -1;

    if (!copy_in(m, t, uptr, len)) {
        object_unref(memobj_object(m));
        return -1;
    }

    handle_t h = publish(m);
    object_unref(memobj_object(m));     /* the handle owns the object now */

    if (h == HANDLE_INVALID)
        return -1;

    *out = h;
    return 0;
}

int ipc_buf_to_user(process_t * t, handle_t h, void *uptr, uint64_t len)
{
    if (t == NULL || uptr == NULL || len == 0)
        return -1;

    process_t *cur = sched_get_current_process();
    if (cur == NULL)
        return -1;

    kernel_object_t *o = handle_get(&cur->handles, h, HANDLE_RIGHT_READ);
    if (o == NULL || o->type != OBJ_MEMORY)
        return -1;

    memobj_t *m = (memobj_t *) o->impl;

    if (len > memobj_size(m))
        len = memobj_size(m);

    bool ok = copy_out(m, t, uptr, len);
    handle_close(&cur->handles, h);

    return ok ? 0 : -1;
}

int ipc_buf_from_kernel(const void *kptr, uint64_t len, handle_t * out)
{
    if (kptr == NULL || len == 0 || out == NULL)
        return -1;

    memobj_t *m = memobj_create(len);
    if (m == NULL)
        return -1;

    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = chunk_len(done, len);
        void *kdst = (void *) (PHYS_TO_VIRT(memobj_page(m, done / PAGE_SIZE))
                               + (done & (PAGE_SIZE - 1)));

        memcpy(kdst, (const uint8_t *) kptr + done, chunk);
        done += chunk;
    }

    handle_t h = publish(m);
    object_unref(memobj_object(m));

    if (h == HANDLE_INVALID)
        return -1;

    *out = h;
    return 0;
}
