/**
 * @file    kmain.c
 * @brief   Entry function of HanOS kernel
 * @details
 *  Finish kernel initializatio and start shell process.
 *  Initial codes are modified from Limine's demo projects: \n
 *  - https://github.com/limine-bootloader/limine-barebones
 * @author  JW
 * @date    Oct 23, 2021
 */

#include <stddef.h>

#include <boot/stivale2.h>

// Tell the stivale bootloader where we want our stack to be.
static uint8_t stack[4096];

// Stivale2 offers a runtime terminal service which can be ditched at any time.
static struct stivale2_header_tag_terminal terminal_hdr_tag = {
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_TERMINAL_ID,
        .next = 0
    },
    .flags = 0
};

// A framebuffer header tag, which is mandatory when using the stivale2 terminal.
static struct stivale2_header_tag_framebuffer framebuffer_hdr_tag = {
    .tag = {
        .identifier = STIVALE2_HEADER_TAG_FRAMEBUFFER_ID,
        .next = (uint64_t)&terminal_hdr_tag
    },
    // We set all the framebuffer specifics to 0 as we want the bootloader
    // to pick the best it can.
    .framebuffer_width  = 800,
    .framebuffer_height = 600,
    .framebuffer_bpp    = 32
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

// This is HanOS kernel's entry point.
void kmain(struct stivale2_struct *stivale2_struct)
{
    struct stivale2_struct_tag_terminal *term_str_tag;
    term_str_tag = stivale2_get_tag(stivale2_struct, STIVALE2_STRUCT_TAG_TERMINAL_ID);

    if (term_str_tag == NULL) {
        for (;;) {
            asm ("hlt");
        }
    }

    void *term_write_ptr = (void *)term_str_tag->term_write;

    void (*term_write)(const char *string, size_t length) = term_write_ptr;

    term_write("Hello World", 11);

    for (;;) {
        asm ("hlt");
    }
}
