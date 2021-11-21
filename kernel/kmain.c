///-----------------------------------------------------------------------------
///
/// @file    kmain.c
/// @brief   Entry function of HanOS kernel
/// @details
///  Finish kernel initializatio and start shell process.
///  1. Initial codes are modified from Limine's demo projects: \n
///     - https://github.com/limine-bootloader/limine-barebones \n
///  2. System initialization to enable terminal outputs
///     - lib: klog system which just realizes printf function.
///     - device: initialize framebuffer based terminal.
/// @author  JW
/// @date    Oct 23, 2021
///
///-----------------------------------------------------------------------------

#include <stddef.h>

#include <3rd-party/boot/stivale2.h>
#include <device/term.h>
#include <lib/klog.h>

// Tell the stivale bootloader where we want our stack to be.
static uint8_t stack[4096];

// Only define framebuffer header tag since we do not want to use stivale2 terminal.
static struct stivale2_header_tag_framebuffer framebuffer_hdr_tag = {
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID,
        .next = 0
    },
    .framebuffer_width  = FB_WIDTH,
    .framebuffer_height = FB_HEIGHT,
    .framebuffer_bpp    = FB_PITCH / FB_WIDTH
};

// According to stivale2 specification, we need to define a "header structure".
__attribute__((section(".stivale2hdr"), used))
static struct stivale2_header stivale_hdr = {
    .entry_point = 0,
    .stack = (uintptr_t)stack + sizeof(stack),
    .flags = (1 << 1),
    .tags = (uintptr_t)&framebuffer_hdr_tag
};

// Scan for tags that we want FROM the bootloader (structure tags).
void *stivale2_get_tag(struct stivale2_struct *stivale2_struct, uint64_t id) {
    struct stivale2_tag *current_tag = (void *)stivale2_struct->tags;
    for (;;) {
        if (current_tag == NULL) {
            return NULL;
        }

        if (current_tag->identifier == id) {
            return current_tag;
        }

        current_tag = (void *)current_tag->next;
    }
}

static term_info_t hanos_term = {0};
static klog_info_t hanos_klog = {0};

// This is HanOS kernel's entry point.
void kmain(struct stivale2_struct* bootinfo)
{
    uint8_t helloworld[] = {0xC4, 0xE3, 0xBA, 0xC3, 0xCA, 0xC0, 0xBD, 0xE7, 0x0};
    // initialize framebuffer and terminal
    term_init(&hanos_term, stivale2_get_tag(bootinfo, STIVALE2_STRUCT_TAG_FRAMEBUFFER_ID));
    klog_init(&hanos_klog, &hanos_term);
    klog_printf(&hanos_klog, "Boot info address: 0x%16x\n", (uint64_t)bootinfo);
    klog_printf(&hanos_klog, "Terminal width: %d, height: %d, pitch: %d\n\n", 
                hanos_term.fb.width, hanos_term.fb.height, hanos_term.fb.pitch);
    klog_printf(&hanos_klog, "\077[11;1m%s\077[0m Hello World", helloworld);
    klog_refresh(&hanos_klog);
    for (;;) {
        asm ("hlt");
    }
}
