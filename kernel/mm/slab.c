/**-----------------------------------------------------------------------------

 @file    slab.c
 @brief   Implementation of slab allocator
 @details
 @verbatim

  This file contains the implementation of a slab allocator for managing
  memory allocation in the HanOS kernel. It defines functions for creating and
  managing slab caches, allocating and freeing memory from these caches, and
  initializing and destructing objects within slabs. The slab allocator
  optimizes memory usage by dividing memory into small, fixed-size chunks and
  reusing them efficiently.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <base/klib.h>
#include <base/spinlock.h>
#include <base/klog.h>
#include <mm/slab.h>
#include <mm/mm.h>
#include <sys/panic.h>
#include <libc/string.h>

#define SLAB_INDIRECT_CUTOFF    256
#define SLAB_INDIRECT_COUNT     8

#define SLAB_PAGE_OFFSET        (PAGE_SIZE - sizeof(slab_t))
#define SLAB_DATA_SIZE          SLAB_PAGE_OFFSET
#define SLAB_INDIRECT_PTR_COUNT (SLAB_DATA_SIZE / sizeof(void **))

#define GET_SLAB(x)             (slab_t *)(ROUND_DOWN((uintptr_t)x, PAGE_SIZE) \
                                + SLAB_PAGE_OFFSET)

#define SLAB_DEBUG              0

/* the cache responsible for allocating all others */
static bool selfcacheinit = false;
static scache_t selfcache = {
    .size = sizeof(scache_t),
    .alignment = 8,
    .truesize = ROUND_UP(sizeof(scache_t) + sizeof(void **), 8),
    .slabobjcount = SLAB_DATA_SIZE / ROUND_UP(sizeof(scache_t)
                                              + sizeof(void **), 8)
};

static void initdirect(scache_t * cache, slab_t * slab, void *base)
{
    slab->free = NULL;
    slab->used = 0;

    for (uintmax_t offset = 0;
         offset < cache->truesize * cache->slabobjcount;
         offset += cache->truesize) {
        if (cache->ctor) {
            cache->ctor(cache, (void *) ((uintptr_t) base + offset));
        }

        void **freenext =
            (void **) ((uintptr_t) base + offset + cache->size);
        *freenext = slab->free;
        slab->free = freenext;
    }
}

static void initindirect(scache_t * cache, slab_t * slab, void **base,
                         void *objbase)
{
    slab->free = NULL;
    slab->used = 0;
    slab->base = objbase;

    for (uintmax_t offset = 0;
         offset < cache->truesize * cache->slabobjcount;
         offset += cache->truesize) {
        if (cache->ctor) {
            cache->ctor(cache, (void *) ((uintptr_t) objbase + offset));
        }

        void **freenext = &base[offset / cache->truesize];
        *freenext = slab->free;
        slab->free = freenext;
    }
}

static bool growcache(scache_t * cache)
{
    void *_slab =
        (void *) PHYS_TO_VIRT(pmm_get(1, 0x0, __func__, __LINE__));
    if (_slab == NULL)
        return false;
    slab_t *slab = GET_SLAB(_slab);

    if (cache->size < SLAB_INDIRECT_CUTOFF) {
        initdirect(cache, slab, _slab);
    } else {
        void *base = (void *)
            PHYS_TO_VIRT(pmm_get
                         (NUM_PAGES(cache->slabobjcount * cache->truesize),
                          0x0, __func__, __LINE__));
        if (base == NULL) {
            pmm_free(VIRT_TO_PHYS(_slab), 1, __func__, __LINE__);
            return false;
        }
        initindirect(cache, slab, _slab, base);
    }

    slab->next = cache->empty;
    slab->prev = NULL;
    if (slab->next) {
        slab->next->prev = slab;
    }
    cache->empty = slab;
    return true;
}

static void *takeobject(scache_t * cache, slab_t * slab)
{
    if (slab->free == NULL) {
        return NULL;
    }

    void **objend = slab->free;

#if SLAB_DEBUG != 0
    if (cache->size < SLAB_INDIRECT_CUTOFF) {
        void *addr = (void *) objend;
        void *base = (void *) ROUND_DOWN((uintptr_t) slab, PAGE_SIZE);
        if (!(addr >= base && addr < (void *) slab)) {
            kpanic("slab: addr 0x%x is not in range 0x%x - 0x%x\n",
                   addr, base, slab);
        }
    } else {
        void *addr = slab->base + ((uintptr_t) objend
                                   - ROUND_DOWN((uintptr_t) slab,
                                                PAGE_SIZE))
            / sizeof(void **) * cache->truesize;
        if (!(addr >= slab->base
              && (uintptr_t) addr < (uintptr_t) slab->base
              + cache->slabobjcount * cache->truesize)) {
            kpanic("slab: addr 0x%x is not in range 0x%x - 0x%x\n",
                   addr, slab->base, (uintptr_t) slab->base
                   + cache->slabobjcount * cache->truesize);
        }
    }
#endif

    slab->free = *slab->free;
    slab->used += 1;
    *objend = NULL;
    if (cache->size < SLAB_INDIRECT_CUTOFF) {
        return (void *) ((uintptr_t) objend - cache->size);
    } else {
        return (void *) ((uintptr_t) slab->base
                         + ((uintptr_t) objend
                            - ROUND_DOWN((uintptr_t) slab, PAGE_SIZE))
                         / sizeof(void **) * cache->truesize);
    }
}

/* frees an object and returns the slab the object belongs to */
static slab_t *returnobject(scache_t * cache, void *obj)
{
    slab_t *slab = NULL;
    void **freeptr = NULL;
    if (cache->size < SLAB_INDIRECT_CUTOFF) {
        slab = (slab_t *) (ROUND_DOWN((uintptr_t) obj, PAGE_SIZE) +
                           SLAB_PAGE_OFFSET);
        freeptr = (void **) ((uintptr_t) obj + cache->size);
        ASSERT(*freeptr == NULL);
    } else {
        bool partial = cache->full == NULL;
        slab = partial ? cache->partial : cache->full;
        while (slab) {
            void *top = (void *) ((uintptr_t) slab->base
                                  + cache->slabobjcount * cache->truesize);
            if (obj >= slab->base && obj < top)
                break;
            if (slab->next == NULL && partial == false) {
                slab = cache->partial;
                partial = true;
            } else {
                slab = slab->next;
            }
        }
        ASSERT(slab);
        uintmax_t objn = ((uintptr_t) obj
                          - (uintptr_t) slab->base) / cache->truesize;
        void **base = (void **) ROUND_DOWN((uintptr_t) slab, PAGE_SIZE);
        freeptr = &base[objn];
    }

    if (cache->dtor)
        cache->dtor(cache, obj);

    *freeptr = slab->free;
    slab->free = freeptr;
    --slab->used;

    return slab;
}

void *slab_allocate(scache_t * cache)
{
    lock_lock(&cache->lock);

    slab_t *slab = NULL;
    if (cache->partial != NULL)
        slab = cache->partial;
    else if (cache->empty != NULL)
        slab = cache->empty;

    void *ret = NULL;

    if (slab == NULL) {
        if (growcache(cache))
            slab = cache->empty;
        else
            goto cleanup;
    }

    ret = takeobject(cache, slab);

    if (slab == cache->empty) {
        cache->empty = slab->next;
        if (slab->next)
            slab->next->prev = NULL;

        if (cache->partial)
            cache->partial->prev = slab;

        slab->next = cache->partial;
        slab->prev = NULL;
        cache->partial = slab;
    } else if (slab->used == cache->slabobjcount) {
        cache->partial = slab->next;

        if (slab->next)
            slab->next->prev = NULL;

        if (cache->full)
            cache->full->prev = slab;

        slab->next = cache->full;
        slab->prev = NULL;
        cache->full = slab;
    }

  cleanup:
    lock_release(&cache->lock);
    return ret;
}

void slab_free(scache_t * cache, void *addr)
{
    lock_lock(&cache->lock);

    slab_t *slab = returnobject(cache, addr);
    ASSERT(slab);

    if (slab->used == 0) {
        if (slab->prev == NULL)
            cache->partial = slab->next;
        else
            slab->prev->next = slab->next;

        if (slab->next)
            slab->next->prev = slab->prev;

        slab->next = cache->empty;
        slab->prev = NULL;
        if (slab->next)
            slab->next->prev = slab;
        cache->empty = slab;
    }

    if (slab->used == cache->slabobjcount - 1) {
        if (slab->prev == NULL)
            cache->full = slab->next;
        else
            slab->prev->next = slab->next;

        if (slab->next)
            slab->next->prev = slab->prev;

        slab->next = cache->partial;
        slab->prev = NULL;
        if (slab->next)
            slab->next->prev = slab;
        cache->partial = slab;
    }

    lock_release(&cache->lock);
}

scache_t *slab_newcache(uint64_t size, uint64_t alignment,
                        void (*ctor)(scache_t *, void *),
                        void(*dtor)(scache_t *, void *))
{
    if(alignment == 0)
        alignment = 8;

    if (selfcacheinit == false) {
        selfcacheinit = true;
        memset((void *) &selfcache.lock, 0, sizeof(lock_t));
    }

    scache_t *cache = slab_allocate(&selfcache);
    if (cache == NULL)
        return NULL;

    cache->size = size;
    cache->alignment = alignment;
    uint64_t freeptrsize =
        size < SLAB_INDIRECT_CUTOFF ? sizeof(void **) : 0;
    cache->truesize = ROUND_UP(size + freeptrsize, alignment);
    cache->ctor = ctor;
    cache->dtor = dtor;
    cache->slabobjcount = size < SLAB_INDIRECT_CUTOFF
        ? SLAB_DATA_SIZE / cache->truesize : SLAB_INDIRECT_COUNT;
    cache->full = NULL;
    cache->empty = NULL;
    cache->partial = NULL;

    memset((void *) &cache->lock, 0, sizeof(lock_t));

    klogd("slab: new cache: size %d align %d true size %d obj count %d\n",
          cache->size, cache->alignment, cache->truesize,
          cache->slabobjcount);

    return cache;
}

static uint64_t purge(scache_t * cache, uint64_t maxcount)
{
    slab_t *slab = cache->empty;
    for (uint64_t done = 0; done < maxcount; ++done) {
        if (slab == NULL)
            return done;

        slab_t *next = slab->next;
        if (next)
            next->prev = NULL;

        if (cache->size >= SLAB_INDIRECT_CUTOFF) {
            pmm_free(VIRT_TO_PHYS(slab->base),
                     NUM_PAGES(cache->slabobjcount * cache->truesize),
                     __func__, __LINE__);
        }

        pmm_free(VIRT_TO_PHYS(slab), 1, __func__, __LINE__);

        slab = cache->empty;
        cache->empty = next;
    }

    return maxcount;
}

void slab_freecache(scache_t * cache)
{
    lock_lock(&cache->lock);

    ASSERT(cache->partial == NULL);
    ASSERT(cache->full == NULL);

    purge(cache, (uint64_t) - 1);

    slab_free(&selfcache, cache);

    lock_release(&cache->lock);
}
