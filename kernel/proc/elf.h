#pragma once

#include <stdint.h>
#include <stddef.h>
#include <proc/task.h>
#include <core/mm.h>

#define ELF_MAGIC       0x464C457FU /* "\x7FELF" in little endian */

#define PT_LOAD         0x00000001  /* Loadable program segment */ 
#define PT_INTERP       0x00000003  /* Program interpreter */
#define PT_PHDR         0x00000006  /* Entry for header table itself */

#define ABI_SYSV        0x00
#define ARCH_X86_64     0x3e
#define BITS_LE         0x01

#define EI_CLASS        0
#define EI_DATA         1
#define EI_VERSION      2
#define EI_OSABI        3
#define EI_ABIVERSION   4

typedef struct {
    uint32_t magic;     /* Magic number and other info */
    uint8_t  elf[12];
    uint16_t type;      /* Object file type */
    uint16_t machine;   /* Architecture */
    uint32_t version;   /* Object file version */
    uint64_t entry;     /* Entry point virtual address */
    uint64_t phoff;     /* Program header table file offset */
    uint64_t shoff;     /* Section header table file offset */
    uint32_t flags;     /* Processor-specific flags */
    uint16_t ehsize;    /* ELF header size in bytes */
    uint16_t phentsize; /* Program header table entry size */
    uint16_t phnum;     /* Program header table entry count */
    uint16_t shentsize; /* Section header table entry size */
    uint16_t shnum;     /* Section header table entry count */
    uint16_t shstrndx;  /* Section header string table index */
} elf_hdr_t;

#define PF_X            1
#define PF_W            2
#define PF_R            4

typedef struct {
    uint32_t type;      /* Segment type */
    uint32_t flags;     /* Segment-dependent flags */
    uint64_t offset;    /* Segment offset in the file image */
    uint64_t vaddr;     /* Segment virtual address in memory */
    uint64_t paddr;     /* Reserved for segment physical address */
    uint64_t filesz;    /* Segment size in file image */
    uint64_t memsz;     /* Segment size in memory */
    uint64_t align;     /* 0 and 1 specify no alignment. Otherwise should be a
                         * positive, integral power of 2, with p_vaddr equating
                         * p_offset modulus p_align.
                         */
} elf_phdr_t;

#define SHT_NULL            0       /* sh_type */
#define SHT_PROGBITS        1
#define SHT_SYMTAB          2
#define SHT_STRTAB          3
#define SHT_RELA            4
#define SHT_HASH            5
#define SHT_DYNAMIC         6
#define SHT_NOTE            7
#define SHT_NOBITS          8
#define SHT_REL             9
#define SHT_SHLIB           10
#define SHT_DYNSYM          11
#define SHT_UNKNOWN12       12
#define SHT_UNKNOWN13       13
#define SHT_INIT_ARRAY      14
#define SHT_FINI_ARRAY      15
#define SHT_PREINIT_ARRAY   16
#define SHT_GROUP           17
#define SHT_SYMTAB_SHNDX    18
#define SHT_NUM             19

typedef struct elf_shdr_t {
    uint32_t name;      /* An offset to a string in the shstrtab section */
    uint32_t type;      /* Section header type */
    uint64_t flags;     /* Section attributes */
    uint64_t addr;      /* Virtual address of the section in memory */
    uint64_t offset;    /* Offset of the section in the file image */
    uint64_t size;      /* Size in bytes of the section in the file image */
    uint32_t link;      /* Contain section index of an associated section */
    uint32_t info;      /* Contain extra information about the section */
    uint64_t addralign; /* Contains the required alignment of the section */
    uint64_t entsize;   /* Contains the size, in bytes, of each entry, for
                         * sections that contain fixed-size entries. Otherwise,
                         *  this field contains zero.
                         */
} elf_shdr_t;

typedef struct {
    uint32_t name_size;
    uint32_t desc_size;
    uint32_t type;
} elf_nhdr_t;

int64_t load_elf(char *path_name, auxval_t *aux);

