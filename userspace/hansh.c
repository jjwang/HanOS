/*
 * There is a tutorial about how to write a shell in C.
 * URL: https://brennan.io/2015/01/16/write-a-shell-in-c/
 */
#include <stddef.h>

#include <libc/string.h>

#include <sysfunc.h>

#define MAX_COMMAND_LEN 1024

#define MSG_WELCOME     "Welcome to HanOS world!"
#define MSG_PROMPT      "\033[36m$ \033[0m"

#define print(x)        sys_write(STDOUT, x, strlen(x), NULL)
#define readkey(x)      sys_read(STDIN, x, 1, NULL)

int main(void)
{
    print("\n\033[31mWelcome to HanOS world!\033[0m "
          "Type \"\033[36mhelp\033[0m\" for command list\n");

    int64_t ret = sys_fork();

    if (ret == 0) {
        while (1) { asm volatile ("nop"); }
    } else if (ret < 0) {
        while (1) { asm volatile ("nop"); }
    } else {
        /* Do nothing */
    }

    /* Run command loop */
    while (1) {
        print(MSG_PROMPT);

        char read_str[MAX_COMMAND_LEN] = {0};
        char write_str[MAX_COMMAND_LEN] = {0};
        char cmd_buff[MAX_COMMAND_LEN] = {0};
        uint16_t cmd_end = 0;

        while (1) {
            int64_t ret = readkey(read_str); 

            if (ret < 0) {
                break;
            } else if (ret == 0) {
                continue;
            }

            read_str[1] = '\0';

            if (read_str[0] == '\n') {
                if (cmd_end > 0) {
                    strupr(cmd_buff);
                    print("Sorry, I cannot understand.\n");
                }

                cmd_buff[0] = '\0';
                cmd_end = 0;

                print(MSG_PROMPT);
            } else if (read_str[0] == '\b') {
                if (cmd_end > 0) {
                    cmd_buff[--cmd_end] = '\0';
                }
            } else if (read_str[0] > 0) {
                if (cmd_end < sizeof(cmd_buff) - 1) {
                    cmd_buff[cmd_end++] = read_str[0];
                    cmd_buff[cmd_end] = '\0';
                }
            }
        } /* End ONE commane */
    }

    while (1) {
        asm volatile ("nop");
    }
}
