/**-----------------------------------------------------------------------------

 @file    kmain.c
 @brief   Entry function of HanOS kernel
 @details
 @verbatim

  Lots of system initializations will be processed here.

  History:
    Feb 19, 2022  Added CLI task which supports some simple commands.

 @endverbatim

 **-----------------------------------------------------------------------------
 */

#include <stddef.h>

#include <3rd-party/boot/stivale2.h>
#include <lib/time.h>
#include <lib/klog.h>
#include <lib/string.h>
#include <lib/shell.h>
#include <core/mm.h>
#include <core/gdt.h>
#include <core/idt.h>
#include <core/smp.h>
#include <core/cmos.h>
#include <core/acpi.h>
#include <core/apic.h>
#include <core/hpet.h>
#include <core/panic.h>
#include <core/pci.h>
#include <core/pit.h>
#include <device/display/term.h>
#include <device/keyboard/keyboard.h>
#include <device/storage/ata.h>
#include <proc/sched.h>
#include <fs/vfs.h>

/* Tell the stivale bootloader where we want our stack to be. */
static uint8_t stack[64000];

/* Only define framebuffer header tag since we do not want to use stivale2 terminal. */
static struct stivale2_header_tag_framebuffer framebuffer_hdr_tag = {
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID,
        .next = 0
    },
    .framebuffer_width  = FB_WIDTH,
    .framebuffer_height = FB_HEIGHT,
    .framebuffer_bpp    = FB_PITCH * 8 / FB_WIDTH
};

/* According to stivale2 specification, we need to define a "header structure". */
__attribute__((section(".stivale2hdr"), used))
static struct stivale2_header stivale_hdr = {
    .entry_point = 0,
    .stack = (uintptr_t)stack + sizeof(stack),
    .flags = 0,
    .tags = (uintptr_t)&framebuffer_hdr_tag
};

/* Scan for tags that we want FROM the bootloader (structure tags). */
void *stivale2_get_tag(struct stivale2_struct *stivale2_struct, uint64_t id) {
    struct stivale2_tag *current_tag = (void *)PHYS_TO_VIRT(stivale2_struct->tags);
    for (;;) {
        if (current_tag == NULL) {
            return NULL;
        }

        if (current_tag->identifier == id) {
            return current_tag;
        }

        current_tag = (void *)PHYS_TO_VIRT(current_tag->next);
    }
}

static volatile enum {
    CURSOR_INVISIBLE = 0,
    CURSOR_VISIBLE,
    CURSOR_HIDE
} cursor_visible = CURSOR_INVISIBLE;

_Noreturn void kcursor(task_id_t tid)
{
    while (true) {
        sleep(500);
        if (cursor_visible == CURSOR_INVISIBLE) {
            term_set_cursor('_');
            cursor_visible = CURSOR_VISIBLE;
        } else if (cursor_visible == CURSOR_VISIBLE) {
            term_set_cursor(' ');
            cursor_visible = CURSOR_INVISIBLE;
        } else {
            term_set_cursor(' ');
        }
        term_refresh(TERM_MODE_CLI);
    }   

    (void)tid;
}

_Noreturn void kshell(task_id_t tid)
{
    (void)tid;
    command_t cmd_list[] = {
        {"memory", pmm_dump_usage, "Dump memory usage information"},
        {"vfs",    vfs_debug,      "Dump tree of virtual file system"},
        {"pci",    pci_debug,      "Print list of PCI devices"},
        {"",       NULL,           ""}};

    klogi("Shell task started\n");
    kprintf("?[11;1m%s?[0m Type \"?[14;1m%s?[0m\" for command list\n",
            "Welcome to HanOS world!", "help");
    kprintf("?[14;1m%s?[0m", "$ ");

    pci_init();
    ata_init();

    char* fn1 = "/disk/0/EFI/BOOT";
    char* fn2 = "/disk/0/HELLOWLD.TXT";
    char* fn3 = "/disk/0/HANOS.TXT";
#if 1
    vfs_handle_t f1 = vfs_open(fn1, VFS_MODE_READ);
    if (f1 != VFS_INVALID_HANDLE) {
        vfs_close(f1);
    } else {
        kloge("Open %s failed\n", fn1);
    }

    vfs_handle_t f2 = vfs_open(fn2, VFS_MODE_READWRITE);
    vfs_handle_t f21 = vfs_open(fn2, VFS_MODE_READWRITE);
    if (f2 != VFS_INVALID_HANDLE) {
        char buff_read[1024] = {0};
        char buff_write[1024] = "(1) This is a test-----------------------------------END";
        vfs_write(f2, strlen(buff_write), buff_write);
        size_t readlen = vfs_read(f2, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn2, f2, buff_read);
        vfs_close(f2);
    } else {
        kloge("Open %s(%d) failed\n", fn2, f21);
    }

    if (f21 != VFS_INVALID_HANDLE) {
        char buff_read[1024] = {0};
        char buff_write[1024] = "(2) This is a test";
        vfs_seek(f21, 10);
        vfs_write(f21, strlen(buff_write), buff_write);
        vfs_seek(f21, 0);
        size_t readlen = vfs_read(f21, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn2, f21, buff_read);
        vfs_close(f21);
    } else {
        kloge("Open %s(%d) failed\n", fn2, f21);
    }

    vfs_handle_t f3 = vfs_open(fn3, VFS_MODE_READ);
    if (f3 != VFS_INVALID_HANDLE) {
        char buff_read[1024] = {0};
        size_t readlen = vfs_read(f3, sizeof(buff_read) - 1, buff_read);
        klogi("Read %d bytes from %s(%d)\n%s\n", readlen, fn3, f3, buff_read);

        vfs_close(f3);
    } else {
        kloge("Open %s failed\n", fn3);
    } 
#endif
    char cmd_buff[1024] = {0};
    uint16_t cmd_end = 0;

    while (true) {
        sleep(16);
        uint8_t cur_key = keyboard_get_key();
        if (term_get_mode() != TERM_MODE_CLI) {
            continue;
        }
        if (cur_key == 0x0A) {
            cursor_visible = CURSOR_HIDE;
            term_set_cursor(' ');
            term_refresh(TERM_MODE_CLI);
            kprintf("%c", cur_key);

            if (cmd_end > 0) {
                if (strcmp(cmd_buff, "help") == 0) {
                    kprintf("%16s%s\n", "Command", "Description");
                    kprintf("%16s%s\n", "-------", "-----------");
                    for(int64_t i = 0; ; i++) {
                        if (cmd_list[i].proc == NULL) break;
                        kprintf("%16s%s\n", cmd_list[i].command, cmd_list[i].desc);
                    }
                } else {
                    for(int64_t i = 0; ; i++) {
                        if (cmd_list[i].proc == NULL) {
                            kprintf("Sorry, I cannot understand.\n");
                            break;
                        } else if (strcmp(cmd_buff, cmd_list[i].command) == 0) {
                            (cmd_list[i].proc)();
                            break;
                        }
                    }
                }
            }

            cursor_visible = CURSOR_INVISIBLE;
            cmd_buff[0] = '\0';
            cmd_end = 0;
            kprintf("?[14;1m%s?[0m", "$ ");
            cursor_visible = CURSOR_INVISIBLE;
        } else if (cur_key == '\b') {
            if (cmd_end > 0) {
                cmd_buff[cmd_end--] = '\0';
                kprintf("?[14;1m%c?[0m", cur_key);
            }   
        }  else if (cur_key > 0) {
            if (cmd_end < sizeof(cmd_buff) - 1) {
                cmd_buff[cmd_end++] = cur_key;
                cmd_buff[cmd_end] = '\0';
            }   
            kprintf("?[14;1m%c?[0m", cur_key);
        }
    }   
}

/* This is HanOS kernel's entry point. */
void kmain(struct stivale2_struct* bootinfo)
{
    uint8_t helloworld[] = {0xC4, 0xE3, 0xBA, 0xC3, 0xCA, 0xC0, 0xBD, 0xE7, 0x0};

    bootinfo = (struct stivale2_struct*)PHYS_TO_VIRT(bootinfo);
    term_init(stivale2_get_tag(bootinfo, STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID));

    klog_init();

    kprintf("?[11;1m%s?[0m Hello World\n\n", helloworld);

    klogi("HanOS version 0.1 starting...\n");
    klogi("Boot info address: 0x%16x\n", (uint64_t)bootinfo);

    pmm_init(stivale2_get_tag(bootinfo, STIVALE2_STRUCT_TAG_MEMMAP_ID));
    vmm_init();

    term_start();

    gdt_init(NULL);
    idt_init();

    /* Need to init after idt_init() because it will be used very often. */
    pit_init();

    keyboard_init();

    acpi_init(stivale2_get_tag(bootinfo, STIVALE2_STRUCT_TAG_RSDP_ID));
    hpet_init();
    cmos_init(); 
    apic_init();

    vfs_init();
    smp_init();

    klogi("Press \"?[14;1m%s?[0m\" (left) to shell and \"?[14;1m%s?[0m\" back\n",
          "ctrl+shift+1", "ctrl+shift+2");

#if 0
    int val1 = 10000, val2 = 0;
    kloge("Val: %d", val1 / val2);
#endif

    sched_add(kshell);
    sched_add(kcursor);

    cpu_t *cpu = smp_get_current_cpu(false);
    if(cpu != NULL) {
        sched_init(cpu->cpu_id);
    } else {
        kpanic("Can not get CPU info in shell process\n");
    }

    /* According to current implementation, the below codes will not be
     * executed.
     */
    for (;;) {
        asm volatile ("hlt;");
    }
}
