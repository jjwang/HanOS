#include <stddef.h>
#include <stdint.h>

#include <lib/string.h>
#include <lib/syscall.h>

int main(void)
{
    char message[128] = "TTY driver loaded\n";
    syscall_entry(SYSCALL_WRITE, STDOUT, message, strlen(message));

    while (1) {
        asm volatile ("nop");
    }
}
