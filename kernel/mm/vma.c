/**-----------------------------------------------------------------------------

 @file    vma.c
 @brief   Virtual memory areas
 @details
 @verbatim

  Maintain a sorted array of user mappings. Each mapping owns a contiguous
  kmalloc chunk. Partial removals clear the requested PTEs; a fully covered
  mapping drops its chunk. Kernel buffers live in the address space owned list
  and never enter this table.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <mm/mm.h>
#include <mm/vma.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>

#define VMA_PAGE_SHIFT  12
#define VMA_PAGE_SIZE   (1ULL << VMA_PAGE_SHIFT)
#define VMA_INIT_CAP    8

static void vma_memmove(void *dst, const void *src, uint64_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d == s || n == 0)
        return;
    if (d < s) {
        for (uint64_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (uint64_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
}

static int vma_grow(vma_map_t * map)
{
    uint32_t cap = map->cap ? map->cap * 2 : VMA_INIT_CAP;
    vma_t *tab = kmalloc(cap * sizeof(vma_t));

    if (tab == NULL)
        return -1;
    if (map->len > 0)
        memcpy(tab, map->tab, map->len * sizeof(vma_t));
    if (map->tab != NULL)
        kmfree(map->tab);
    map->tab = tab;
    map->cap = cap;
    return 0;
}

void vma_init(vma_map_t * map)
{
    memset(map, 0, sizeof(*map));
}

uint32_t vma_count(const vma_map_t * map)
{
    return map->len;
}

int vma_add(vma_map_t * map, uint64_t start, uint64_t end, uint64_t paddr,
            uint64_t flags)
{
    uint32_t pos = 0;

    if (start >= end || (start & (VMA_PAGE_SIZE - 1))
        || (end & (VMA_PAGE_SIZE - 1)))
        return -1;

    while (pos < map->len && map->tab[pos].start < start)
        pos++;

    if (pos < map->len && map->tab[pos].start < end)
        return -1;
    if (pos > 0 && map->tab[pos - 1].end > start)
        return -1;

    if (map->len == map->cap && vma_grow(map) != 0)
        return -1;

    vma_memmove(&map->tab[pos + 1], &map->tab[pos],
            (map->len - pos) * sizeof(vma_t));
    map->tab[pos].start = start;
    map->tab[pos].end = end;
    map->tab[pos].paddr = paddr;
    map->tab[pos].flags = flags;
    map->len++;
    return 0;
}

/* Remove [start, end). A fully covered mapping is unmapped and its chunk
 * freed. A partial overlap only clears the requested PTEs; the mapping stays
 * until a later full removal or address space destruction. */
void vma_remove_range(struct addrspace *as, vma_map_t * map, uint64_t start,
                      uint64_t end)
{
    uint32_t i = 0;

    if (start >= end)
        return;

    while (i < map->len) {
        vma_t *v = &map->tab[i];

        if (v->end <= start || v->start >= end) {
            i++;
            continue;
        }

        if (v->start >= start && v->end <= end) {
            uint64_t np = (v->end - v->start) / VMA_PAGE_SIZE;

            vmm_unmap(as, v->start, np);
            kmfree_chunk((void *) PHYS_TO_VIRT(v->paddr), __func__, __LINE__);
            vma_memmove(&map->tab[i], &map->tab[i + 1],
                    (map->len - i - 1) * sizeof(vma_t));
            map->len--;
            continue;
        }

        uint64_t lo = (v->start > start) ? v->start : start;
        uint64_t hi = (v->end < end) ? v->end : end;

        vmm_unmap(as, lo, (hi - lo) / VMA_PAGE_SIZE);
        i++;
    }

    if (map->len == 0) {
        if (map->tab != NULL)
            kmfree(map->tab);
        map->tab = NULL;
        map->cap = 0;
    }
}

void vma_destroy(struct addrspace *as, vma_map_t * map)
{
    for (uint32_t i = 0; i < map->len; i++) {
        vma_t *v = &map->tab[i];

        vmm_unmap(as, v->start, (v->end - v->start) / VMA_PAGE_SIZE);
        kmfree_chunk((void *) PHYS_TO_VIRT(v->paddr), __func__, __LINE__);
    }

    if (map->tab != NULL)
        kmfree(map->tab);
    vma_init(map);
}

int vma_clone(struct addrspace *dst_as, const vma_map_t * src,
              vma_map_t * dst)
{
    vma_init(dst);

    for (uint32_t i = 0; i < src->len; i++) {
        const vma_t *v = &src->tab[i];
        uint64_t np = (v->end - v->start) / VMA_PAGE_SIZE;
        void *buf;

        if (np == 0)
            continue;

        buf = kmalloc_chunk(np * VMA_PAGE_SIZE, __func__, __LINE__);
        if (buf == NULL)
            goto fail;

        uint64_t paddr = VIRT_TO_PHYS(buf);

        memcpy((void *) PHYS_TO_VIRT(paddr),
               (void *) PHYS_TO_VIRT(v->paddr), np * VMA_PAGE_SIZE);
        vmm_map(dst_as, v->start, paddr, np, v->flags);

        if (vma_add(dst, v->start, v->end, paddr, v->flags) != 0) {
            kmfree_chunk(buf, __func__, __LINE__);
            goto fail;
        }
    }

    return 0;

  fail:
    vma_destroy(dst_as, dst);
    return -1;
}

void addrspace_owned_add(addrspace_t * as, const mem_map_t * m)
{
    vec_push_back(&as->owned_list, *m);
}

void addrspace_owned_destroy(addrspace_t * as)
{
    for (uint32_t i = 0; i < vec_length(&as->owned_list); i++) {
        mem_map_t m = vec_at(&as->owned_list, i);

        kmfree_chunk((void *) PHYS_TO_VIRT(m.paddr), __func__, __LINE__);
    }
    vec_erase_all(&as->owned_list);
}

int addrspace_owned_clone(addrspace_t * dst, addrspace_t * src)
{
    for (uint32_t i = 0; i < vec_length(&src->owned_list); i++) {
        mem_map_t m = vec_at(&src->owned_list, i);
        void *buf;

        if (m.np == 0)
            continue;
        buf = kmalloc_chunk(m.np * PAGE_SIZE, __func__, __LINE__);
        if (buf == NULL)
            return -1;
        memcpy(buf, (void *) PHYS_TO_VIRT(m.paddr), m.np * PAGE_SIZE);
        m.paddr = VIRT_TO_PHYS(buf);
        vec_push_back(&dst->owned_list, m);
    }
    return 0;
}
