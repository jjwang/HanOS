#include <base/lock.h>
#include <base/klog.h>

void lock_lock_impl(lock_t *s, const char *fn, const int ln)
{
    (void)fn;
    (void)ln;

    asm volatile(
        "pushfq;"
        "cli;"
        "lock btsl $0, %[lock];"
        "jnc 2f;"
        "1:"
        "pause;"
        "btl $0, %[lock];"
        "jc 1b;"
        "lock btsl $0, %[lock];"
        "jc 1b;"
        "2:"
        "pop %[flags]"
        : [lock] "=m"((s)->lock), [flags] "=m"((s)->rflags)
        :
        : "memory", "cc");
}

void lock_release_impl(lock_t *s, const char *fn, const int ln)
{
    (void)fn;
    (void)ln;

    asm volatile("push %[flags];"
                 "lock btrl $0, %[lock];"
                    "popfq;"
                 : [lock] "=m"((s)->lock)
                 : [flags] "m"((s)->rflags)
                 : "memory", "cc");
}

