#include <proc/elf.h>
#include <proc/task.h>
#include <lib/klib.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <lib/string.h>
#include <fs/vfs.h>
#include <sys/mm.h>
#include <sys/panic.h>

#define RTDL_ADDR       0x40000000

#define IS_TEXT(p)      (p.flags & PF_X)
#define IS_DATA(p)      (p.flags & PF_W)
#define IS_BSS(p)       (p.filesz < p.memsz)

static bool debug_info = false;

int elf_find_symbol_table(elf_hdr_t *hdr, elf_shdr_t *shdr)
{
    for (size_t i = 0; i < hdr->shnum; i++) {
        if (shdr[i].type == SHT_SYMTAB) {
            return i;
        }
    }

    return -1;
}

void *elf_find_sym(const char *name, elf_shdr_t *shdr, elf_shdr_t *shdr_sym,
    const char *src, char *dst)
{
    elf_sym_t *syms = (elf_sym_t*)(src + shdr_sym->offset);
    const char* strings = src + shdr[shdr_sym->link].offset;
    
    for (size_t i = 0; i < shdr_sym->size / sizeof(elf_sym_t); i += 1) {
        if (strcmp(name, strings + syms[i].name) == 0) {
            return dst + syms[i].value;
        }
    }

    return NULL;
}

/* Need to free aux->phaddr after calling elf_load()  ... */
int64_t elf_load(
    task_t *task, const char *path_name, uint64_t *entry, auxval_t *aux)
{
    uint8_t *elf_buff = NULL;
    size_t elf_len = 0;

    bool has_dynamic_linking = false;
    auxval_t dynamic_aux = {0};

    elf_phdr_t *phdr = NULL;
    elf_shdr_t *shdr = NULL;
    uint64_t *phaddr = NULL;

    const char* fn = path_name;
    /* TODO: Need to review const description */
    vfs_handle_t f = vfs_open((char*)fn, VFS_MODE_READWRITE);
    if (f != VFS_INVALID_HANDLE) {
        elf_len = vfs_tell(f);
        elf_buff = (uint8_t*)kmalloc(elf_len);
        if (elf_buff != NULL) {
            size_t readlen = vfs_read(f, elf_len, elf_buff);
            if (debug_info) {
                klogi("ELF(%s): read %d bytes from %s(%d)\n",
                      path_name, readlen, fn, f);
            } 
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

    aux->entry = hdr.entry;
    if (hdr.type == ET_SHARED) aux->entry += RTDL_ADDR;
    aux->phdr = 0;
    aux->phnum = hdr.phnum;
    aux->phentsize = hdr.phentsize;

    if (debug_info) {
        klogi("ELF(%s): entry address 0x%x, type %d, size %d (%d)\n",
              path_name, hdr.entry, hdr.type, hdr.phentsize,
              sizeof(elf_phdr_t));
    }

    phdr = kmalloc(hdr.phnum * sizeof(elf_phdr_t));
    if (!phdr) goto err_exit;
    memcpy(phdr, elf_buff + hdr.phoff, hdr.phnum * sizeof(elf_phdr_t));

    phaddr = (uint64_t*)kmalloc(hdr.phnum * sizeof(uint64_t));
    if (phaddr == NULL)                 goto err_exit;
    aux->phaddr = (uint64_t)phaddr;

    for (size_t i = 0; i < hdr.phnum; i++) {
        phaddr[i] = (uint64_t)NULL;

        if (phdr[i].type == PT_INTERP && phdr[i].filesz > 0
            && !has_dynamic_linking)
        {
            char *rdtl_path = (char*)kmalloc(phdr[i].filesz + 1);
            if (rdtl_path != NULL) {
                memcpy(rdtl_path, &(elf_buff[phdr[i].offset]), phdr[i].filesz);
                rdtl_path[phdr[i].filesz] = '\0';
                if (debug_info) {
                    klogi("ELF(%s): %d hdr has dynamic linking from %s\n",
                          path_name, i, rdtl_path);
                }
                has_dynamic_linking = true;
                elf_load(task, rdtl_path, NULL, &dynamic_aux);
                kmfree(rdtl_path);
            }
            continue;
        }

        if (phdr[i].type == PT_PHDR) {
            if (debug_info) {
                klogi("ELF(%s): %d hdr is entry for header table itself "
                      "(paddr 0x%x, vaddr 0x%x)\n",
                      path_name, i, phdr[i].paddr, phdr[i].vaddr);
            }
            aux->phdr = phdr[i].vaddr;
            if (hdr.type == ET_SHARED) aux->phdr += RTDL_ADDR;
            continue;
        }

        if (phdr[i].type != PT_LOAD) {
            if (debug_info) {
                klogi("ELF(%s): %d hdr is not load header (type 0x%x)\n",
                      path_name, i, phdr[i].type);
            }
            continue;
        }

        /* In the below part of this loop, only PT_LOAD will be processed. */
        if (debug_info ) {
            if (IS_TEXT(phdr[i])) {
                klogi("ELF(%s): %d hdr is text program header <<<\n",
                      path_name, i);
            } else if (IS_DATA(phdr[i])) {
                klogi("ELF(%s): %d hdr is data program header <<<\n",
                      path_name, i);
            }

            if (IS_BSS(phdr[i])) {
                klogi("ELF(%s): %d hdr is bss program header <<<\n",
                      path_name, i);
            }
        }

        size_t misalign = phdr[i].vaddr & (PAGE_SIZE - 1);
        size_t page_count = DIV_ROUNDUP(misalign + phdr[i].memsz, PAGE_SIZE);

        uint64_t addr = pmm_get(page_count, 0x0);
        if (!addr) {
            kpanic("ELF(%s): cannot alloc %d bytes memory",
                   path_name, page_count * PAGE_SIZE);
        }
        phaddr[i] = addr;

        size_t pf = VMM_FLAG_PRESENT | VMM_FLAGS_USERMODE;
        if(phdr[i].flags & PF_W) {
            pf |= VMM_FLAG_READWRITE;
        }

        uint64_t virt = phdr[i].vaddr - misalign;
        if (hdr.type == ET_SHARED) virt += RTDL_ADDR;

        vmm_map(task->addrspace, virt, addr, page_count, pf, false);

        /*
         * It is better if we set initialized data to zero which is also a NULL
         * pointer.
         */
        memset((void*)PHYS_TO_VIRT(addr), 0x00, PAGE_SIZE * page_count);

        if (debug_info) {
            klogi("ELF(%s): as 0x%x - %d bytes, map 0x%11x to virt 0x%x, "
                  "PML4 0x%x, page count %d\n",
                  path_name, task->addrspace, phdr[i].memsz, addr, virt,
                  task->addrspace == NULL ? NULL : task->addrspace->PML4,
                  page_count);
        }

        mem_map_t m;
        m.vaddr = virt;
        m.paddr = addr;
        m.np = page_count;
        m.flags = pf;

        vec_push_back(&task->mmap_list, m);

        memcpy((void*)PHYS_TO_VIRT(addr + misalign), elf_buff + phdr[i].offset,
               phdr[i].filesz);

        if (debug_info) {
            klogi("ELF(%s): %d hdr's task binary file size %d "
                  "(mem size %d) bytes >>>\n",
                  path_name, i, phdr[i].filesz, phdr[i].memsz);
        }
        /* Need to free in some other places */
    }

    shdr = kmalloc(hdr.shnum * sizeof(elf_shdr_t));
    if (!shdr) goto err_exit;

    aux->shdr = (uint64_t)shdr;
    memcpy(shdr, elf_buff + hdr.shoff, hdr.shnum * sizeof(elf_shdr_t));

    char *header_strs = (char*)&elf_buff[shdr[hdr.shstrndx].offset];
    if (debug_info) {
        for (size_t k = 0; k < hdr.shnum && debug_info; k++) {
            klogi("ELF(%s): %d 0x%x type %d \"%s\", offset %d, size %d\n",
                  path_name, k, shdr[k].addr, shdr[k].type,
                  &header_strs[shdr[k].name], shdr[k].offset, shdr[k].size);
        }
    }

    int symbol_table_index = elf_find_symbol_table(&hdr, shdr);

    elf_shdr_t *shdr_sym = (elf_shdr_t*)(shdr + symbol_table_index);
    elf_sym_t *syms = (elf_sym_t*)((uint8_t*)elf_buff + shdr_sym->offset);
    const char* strings = (char*)elf_buff + shdr[shdr_sym->link].offset;
        
    for (size_t i = 0; i < shdr_sym->size / sizeof(elf_sym_t); i += 1) {
        if (strcmp("main", strings + syms[i].name) == 0) {
            klogi("ELF(%s): Found entry function (main) with len %d, "
                  "session idx %d, value 0x%x\n",
                  path_name, syms[i].size, syms[i].shndx, syms[i].value);
        }   
    }   

    if (has_dynamic_linking) {
        if (entry != NULL) *entry = dynamic_aux.entry;
    } else {
        if (entry != NULL) *entry = aux->entry;
    }

    klogi("ELF(%s): Read header with phnum %d, shnum %d, entry 0x%x\n",
          path_name, hdr.phnum, hdr.shnum, hdr.entry);

    /* TODO: Need to consider when to free phdr, phaddr, shdr and elf_buff */
#if 0
    if (phdr)       kmfree(phdr);
    if (shdr)       kmfree(shdr);
    if (elf_buff)   kmfree(elf_buff);
#endif

    return 0;

err_exit:
    kloge("ELF(%s): File header error\n", path_name);

    if (phdr)       kmfree(phdr);
    if (phaddr)     kmfree(phaddr);
    if (shdr)       kmfree(shdr);
    if (elf_buff)   kmfree(elf_buff);

    return -1;
}

