#pragma once

#include <stdint.h>
#include <stddef.h>
#include <proc/task.h>
#include <core/mm.h>

#define ELF_MAGIC   0x464C457FU  /* "\x7FELF" in little endian */

#define PT_LOAD     0x00000001
#define PT_INTERP   0x00000003
#define PT_PHDR     0x00000006

#define ABI_SYSV    0x00
#define ARCH_X86_64 0x3e
#define BITS_LE     0x01

#define EI_CLASS    0
#define EI_DATA     1
#define EI_VERSION  2
#define EI_OSABI    3

typedef struct {
    uint32_t magic;
    uint8_t  elf[12];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t hdr_size;
    uint16_t phdr_size;
    uint16_t ph_num;
    uint16_t shdr_size;
    uint16_t sh_num;
    uint16_t shstrndx;
} elf_hdr_t;

#define PF_X        1
#define PF_W        2
#define PF_R        4

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t file_size;
    uint64_t mem_size;
    uint64_t align;
} elf_phdr_t;

typedef struct elf_shdr_t {
    uint32_t name;
    uint32_t type;
    uint64_t flags;
    uint64_t addr;
    uint64_t offset;
    uint64_t size;
    uint32_t link;
    uint32_t info;
    uint64_t addr_align;
    uint64_t ent_size;
} elf_shdr_t;

int64_t load_elf(char *path_name);

