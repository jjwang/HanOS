#include <stddef.h>
#include <stdint.h>

#include <lib/string.h>
#include <lib/syscall.h>

#define MSG_WELCOME     "Welcome to HanOS world!"

int main(void)
{
#if 0
    char message[128] = "\e[31mWelcome to HanOS world!\e[0m Type \"\e[36mhelp\e[0m\" for command list\n";
    syscall_entry(SYSCALL_WRITE, STDOUT, message, strlen(message));
#endif
    char prompt[128] = "\e[36m$ \e[0m";
    syscall_entry(SYSCALL_WRITE, STDOUT, prompt, strlen(prompt));

    char read_str[256] = {0}, write_str[256] = {0};
    char cmd_buff[1024] = {0};
    uint16_t cmd_end = 0;

    while (1) {
        int64_t ret = syscall_entry(SYSCALL_READ, STDIN, read_str, 1); 

        if (ret < 0) {
            break;
        } else if (ret == 0) {
            continue;
        }   

        read_str[1] = '\0';

        if (read_str[0] == 0x0A) {
            syscall_entry(SYSCALL_WRITE, STDOUT, read_str, 1); 

            if (cmd_end > 0) {
                syscall_entry(SYSCALL_WRITE, STDOUT, cmd_buff, strlen(cmd_buff));
                syscall_entry(SYSCALL_WRITE, STDOUT, read_str, 1);
            }

            cmd_buff[0] = '\0';
            cmd_end = 0;

            syscall_entry(SYSCALL_WRITE, STDOUT, prompt, strlen(prompt));
        } else if (read_str[0] == '\b') {
            syscall_entry(SYSCALL_WRITE, STDOUT, read_str, 1);

            if (cmd_end > 0) {
                cmd_buff[--cmd_end] = '\0';
            }
        } else if (read_str[0] > 0) {
            if (cmd_end < sizeof(cmd_buff) - 1) {
                cmd_buff[cmd_end++] = read_str[0];
                cmd_buff[cmd_end] = '\0';
            }
            syscall_entry(SYSCALL_WRITE, STDOUT, read_str, 1);
        }
    }

    while (1) {
        asm volatile ("nop");
    }
}
