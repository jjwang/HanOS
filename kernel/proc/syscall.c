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
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/eventbus.h>
#include <device/keyboard/keyboard.h>

extern int64_t syscall_handler();

typedef int64_t (*syscall_ptr_t)(void);

int64_t k_not_implemented()
{
    return -1;
}

int64_t k_read(int fd, void* buf, size_t count)
{
    if (fd == STDIN) {
        if (count <= 0) {
            return 0;
        }

        event_para_t para = 0;
        if (eb_subscribe(sched_get_tid(), EVENT_KEY_PRESSED, &para)) {
            uint8_t keycode = para & 0xFF;
            if (keycode) {
                ((uint8_t*)buf)[0] = keycode;
                return 1;
            }
        }

        return 0;
    }

    if (fd < 3) {
        klogd("k_read: invalid file descriptor fd=%d\n", fd);
        return -EPERM;
    }

    return -EBADF;
}

int64_t k_write(int fd, const void* buf, size_t count)
{
    if (fd == STDOUT || fd == STDERR) {
        char *msg = (char*)kmalloc(count + 1);
        if (msg != NULL) {
            msg[count] = '\0';
            memcpy(msg, buf, count);

            cursor_visible = CURSOR_HIDE;
            term_set_cursor(' ');
            term_refresh(TERM_MODE_CLI, false);

            kprintf("%s", msg);
            cursor_visible = CURSOR_INVISIBLE;

            kmfree(msg);
            return count;
        } else {
            return 0;
        }
    }

    if (fd < 3) {
        klogd("k_write: invalid file descriptor fd=%d\n", fd);
        return -EPERM;
    }

    return -EBADF;
}

syscall_ptr_t syscall_funcs[] = {
    (syscall_ptr_t)k_not_implemented,
    [SYSCALL_READ]  = (syscall_ptr_t)k_read,
    [SYSCALL_WRITE] = (syscall_ptr_t)k_write
};

void syscall_init(void)
{
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1); /* Enable syscall */

    uint64_t star = (uint64_t)DEFAULT_KMODE_CODE << 32;
    star |= (uint64_t)(DEFAULT_KMODE_DATA | 3) << 48;

    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t)&syscall_handler);
    write_msr(MSR_SFMASK, X86_EFLAGS_TF | X86_EFLAGS_DF | X86_EFLAGS_IF
        | X86_EFLAGS_IOPL | X86_EFLAGS_AC | X86_EFLAGS_NT);

    klogi("SYSCALL: MSR_EFER=0x%016x MSR_STAR=0x%016x MSR_LSTAR=0x%016x\n",
          read_msr(MSR_EFER),
          read_msr(MSR_STAR),
          read_msr(MSR_LSTAR));
}
