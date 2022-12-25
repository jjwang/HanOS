#include <stddef.h>
#include <stdint.h>

#include <lib/string.h>
#include <lib/syscall.h>

int main(void)
{
#if 0
    char message[128] = "Input server started\n";
    syscall_entry(SYSCALL_WRITE, STDOUT, message, strlen(message));

    char read_str[128] = {0};

    while (1) {
        int64_t ret = syscall_entry(SYSCALL_READ, STDIN, read_str, 1); 

        if (ret < 0) {
            break;
        } else if (ret == 0) {
            continue;
        }   

        read_str[1] = '\0';

        /* Send key input to event bus */
    }
#endif

    while (1) {
        asm volatile ("nop");
    }
}

