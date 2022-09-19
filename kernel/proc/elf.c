#include <proc/elf.h>
#include <proc/task.h>
#include <lib/klib.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <fs/vfs.h>
#include <core/mm.h>
#include <core/panic.h>

#define BASE    0xffffffff80000000

#define IS_TEXT(p)  (p.flags & PF_X)
#define IS_DATA(p)  (p.flags & PF_W)
#define IS_BSS(p)   (p.filesz < p.memsz)

/* Need to free aux->phdr, aux->phaddr, aux->shdr ... */
int64_t load_elf(char *path_name, auxval_t *aux)
{
    uint8_t *elf_buff = NULL;
    size_t elf_len = 0;

    elf_phdr_t *phdr = NULL;
    elf_shdr_t *shdr = NULL;
    uint64_t *phaddr = NULL;

    char* fn = path_name;
    vfs_handle_t f = vfs_open(fn, VFS_MODE_READWRITE);
    if (f != VFS_INVALID_HANDLE) {
        elf_len = vfs_tell(f);
        elf_buff = (uint8_t*)kmalloc(elf_len);
        if (elf_buff != NULL) {
            size_t readlen = vfs_read(f, elf_len, elf_buff);
            klogi("ELF: read %d bytes from %s(%d)\n", readlen, fn, f); 
        }
        vfs_close(f);
    }

    if (elf_buff == NULL)               goto err_exit;

    elf_hdr_t hdr = {0};
    memcpy(&hdr, elf_buff, sizeof(elf_hdr_t));

    if (hdr.magic != ELF_MAGIC)         goto err_exit;
    if (hdr.elf[EI_CLASS] != 0x02)      goto err_exit;
    if (hdr.elf[EI_DATA] != BITS_LE)    goto err_exit;
    if (hdr.elf[EI_OSABI] != ABI_SYSV)  goto err_exit;
    if (hdr.machine != ARCH_X86_64)     goto err_exit;

    aux->entry = BASE + hdr.entry;
    aux->phnum = hdr.phnum;
    aux->phentsize = hdr.phentsize;

    phdr = kmalloc(hdr.phnum * sizeof(elf_phdr_t));
    if (!phdr) goto err_exit;

    aux->phdr = (uint64_t)phdr;
    memcpy(phdr, elf_buff + hdr.phoff, hdr.phnum * sizeof(elf_phdr_t));

    phaddr = (uint64_t*)kmalloc(hdr.phnum * sizeof(uint64_t));
    if (phaddr == NULL)                 goto err_exit;
    aux->phaddr = (uint64_t)phaddr;

    for (size_t i = 0; i < hdr.phnum; i++) {
        phaddr[i] = (uint64_t)NULL;

        if (phdr[i].type != PT_LOAD) {
            continue;
        }

        if (IS_TEXT(phdr[i])) {
            klogi("ELF: %d hdr is text program header\n", i);
        } else if (IS_DATA(phdr[i])) {
            klogi("ELF: %d hdr is data program header\n", i);
        } if (IS_BSS(phdr[i])) {
            klogi("ELF: %d hdr is bss program header\n", i);
        }

        size_t misalign = phdr[i].vaddr & (PAGE_SIZE - 1);
        size_t page_count = DIV_ROUNDUP(misalign + phdr[i].memsz, PAGE_SIZE);

        uint64_t addr = pmm_get(page_count, 0x0);
        if (!addr) {
            kpanic("ELF: cannot alloc %d bytes memory", page_count * PAGE_SIZE);
        }
        phaddr[i] = addr;

        size_t pf = VMM_FLAG_PRESENT | VMM_FLAG_USER;
        if(phdr[i].flags & PF_W) {
            pf |= VMM_FLAG_READWRITE;
        }

        for (size_t j = 0; j < page_count; j++) {
            uint64_t virt = BASE + (phdr[i].vaddr + (j * PAGE_SIZE));
            uint64_t phys = addr + (j * PAGE_SIZE);
            vmm_map(NULL, virt, phys, 1, pf);
            memset((void*)virt, 0, PAGE_SIZE);
            klogi("ELF: %d bytes, map 0x%11x to virt 0x%x\n", phdr[i].memsz, phys, virt);
        }

        char *buf = (char *)(BASE + phdr[i].vaddr);
        memcpy(buf + misalign, elf_buff + phdr[i].offset, phdr[i].filesz);

        /* Need to free in some other places */
    }

    shdr = kmalloc(hdr.shnum * sizeof(elf_shdr_t));
    if (!shdr) goto err_exit;

    aux->shdr = (uint64_t)shdr;
    memcpy(shdr, elf_buff + hdr.shoff, hdr.shnum * sizeof(elf_shdr_t));

    char *header_strs = (char*)&elf_buff[shdr[hdr.shstrndx].offset];
    for (size_t k = 0; k < hdr.shnum; k++) {
        /* if (shdr[k].type != SHT_SYMTAB) continue; */

        klogi("ELF: %d section 0x%x type %d name \"%s\"\n", k,
              shdr[k].addr, shdr[k].type, &header_strs[shdr[k].name]);
    }

    if (!elf_buff)  kmfree(elf_buff);

    klogi("ELF: Read header with phnum %d, shnum %d, entry 0x%x\n",
          hdr.phnum, hdr.shnum, hdr.entry);
    return 0;

err_exit:
    if (!phdr)      kmfree(phdr);
    if (!phaddr)    kmfree(phaddr);
    if (!shdr)      kmfree(shdr);
    if (!elf_buff)  kmfree(elf_buff);

    return -1;
}

