#include <proc/elf.h>
#include <proc/task.h>
#include <lib/klib.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <fs/vfs.h>
#include <core/mm.h>
#include <core/panic.h>

int64_t load_elf(char *path_name)
{
    uint8_t *elf_buff = NULL;
    size_t elf_len = 0;

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

    elf_phdr_t *phdr = kmalloc(hdr.ph_num * sizeof(elf_phdr_t));
    if (!phdr) goto err_exit;

    if (!phdr)      kmfree(phdr);
    if (!elf_buff)  kmfree(elf_buff);

    klogi("ELF: Read header with ph_num = %d\n", hdr.ph_num);
    return 0;

err_exit:
    if (!phdr)      kmfree(phdr);
    if (!elf_buff)  kmfree(elf_buff);

    return -1;
}

