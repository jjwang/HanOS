#include <lib/lock.h>

void lock_lock(lock_t *s)
{
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

void lock_release(lock_t *s)
{
    asm volatile("push %[flags];"
                 "lock btrl $0, %[lock];"
                    "popfq;"
                 : [lock] "=m"((s)->lock)
                 : [flags] "m"((s)->rflags)
                 : "memory", "cc");
}

