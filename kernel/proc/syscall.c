/* Ref: Page 2994 in Intel® 64 and IA-32 Architectures Software Developer’s
 * Manual Combined Volumes: 1, 2A, 2B, 2C, 2D, 3A, 3B, 3C, 3D, and 4
 */
#include <core/cpu.h>
#include <core/idt.h>
#include <core/apic.h>
#include <core/panic.h>
#include <core/isr_base.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <proc/task.h>
#include <proc/syscall.h>

extern int64_t syscall_handler();

typedef int64_t (*syscall_ptr_t)(void);

int64_t k_not_implemented()
{
    return -1;
}

size_t k_write(int fd, const void* buf, size_t count)
{
    if (fd == STDOUT || fd == STDERR) {
        char *msg = (char*)kmalloc(count + 1);
        if (msg != NULL) {
            msg[count] = '\0';
            memcpy(msg, buf, count);
            klogi("k_write: %s(0x%x, len %d)\n", msg, (uint64_t)buf, count);
            kmfree(msg);
            return 0;
        }
    }

    return -1;
}

syscall_ptr_t syscall_funcs[] = {
    (syscall_ptr_t)k_not_implemented,
    [SYSCALL_WRITE] = (syscall_ptr_t)k_write
};

void syscall_init(void)
{
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1); /* Enable syscall */

    uint64_t star = read_msr(MSR_STAR);

    star = (uint64_t)DEFAULT_KMODE_CS << 32;
    star |= (uint64_t)(DEFAULT_KMODE_SS | 0b00000011) << 48;

    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t)&syscall_handler);
    write_msr(MSR_SFMASK, X86_EFLAGS_TF | X86_EFLAGS_DF | X86_EFLAGS_IF
        | X86_EFLAGS_IOPL | X86_EFLAGS_AC | X86_EFLAGS_NT);

    klogi("SYSCALL: MSR_EFER=0x%016x MSR_STAR=0x%016x MSR_LSTAR=0x%016x\n",
          read_msr(MSR_EFER),
          read_msr(MSR_STAR),
          read_msr(MSR_LSTAR));
}
