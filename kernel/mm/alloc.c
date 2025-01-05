/**-----------------------------------------------------------------------------

 @file    alloc.c
 @brief   Implementation of memory allocation functions
 @details
 @verbatim

  This file contains the implementation of memory allocation functions for the
  HanOS kernel. The allocator uses a slab allocation mechanism to manage memory
  with different cache sizes. Functions for initializing the allocator, as well
  as allocating, reallocating, and freeing memory are provided. Each allocation
  includes metadata for capacity and size, and optionally a poison value for
  debugging purposes.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <mm/mm.h>
#include <mm/alloc.h>
#include <mm/slab.h>
#include <base/klib.h>
#include <base/klog.h>
#include <libc/string.h>

/* each allocation has the following structure:
 * ptr: data capacity (allocsizes size)
 * ptr + sizeof(uint64_t): current size
 * ptr + sizeof(uint64_t) * 2: data
 * ptr + datasize: poison value
 */

#define USE_POISON              0
#define POISON_VALUE            0xdeadbeefbadc0ffel
#define CACHE_COUNT             12

#define CAPACITY_SIZE(cache)    (cache->size - sizeof(uint64_t) * 2 \
                                - USE_POISON * sizeof(uint64_t))

static uint64_t allocsizes[CACHE_COUNT] =
{
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768,
    ALLOC_MAX_SIZE,
};

static scache_t *caches[CACHE_COUNT] = {0};

static void initarea(scache_t *cache, void *obj)
{
    uint64_t *ptr = obj;
    *ptr = CAPACITY_SIZE(cache);
    memset(ptr + 2, 0, *ptr);
#if USE_POISON == 1
    *((uint64_t *)((uintptr_t)obj + cache->size - sizeof(uint64_t))) = POISON_VALUE;
#endif
}

static void dtor(scache_t *cache, void *obj)
{
#if USE_POISON == 1
    ASSERT(*(uint64_t *)((uintptr_t)obj + cache->size - sizeof(uint64_t))
           == POISON_VALUE);
#endif
    initarea(cache, obj);
}

static scache_t *getcachefromsize(uint64_t size)
{
    scache_t *cache = NULL;
    for (int i = 0; i < CACHE_COUNT; ++i) {
        if (size <= allocsizes[i]) {
            cache = caches[i];
            break;
        }
    }
    if (cache == NULL) {
        kpanic("alloc: cannot get cache for size %d\n", size);
    }
    return cache;
}

void *alloc(uint64_t size)
{
    scache_t *cache = getcachefromsize(size);
    uint64_t *ret = slab_allocate(cache);
    if (ret == NULL)
        return NULL;
    ASSERT(*ret == CAPACITY_SIZE(cache));
    *(ret + 1) = size;
    return (void*)(ret + 2);
}

void free(void *ptr)
{
    uint64_t *start = (void*)ptr;
    start -= 2;
    uint64_t size = *start;
    scache_t *cache = getcachefromsize(size);
    slab_free(cache, start);
}

void *realloc(void *ptr, uint64_t size)
{
    uint64_t *start = (void*)ptr;
    start -= 2;
    uint64_t currentsize = *(start + 1);
    if (size <= currentsize) {
        *(start + 1) = size;
        return (void*)ptr;
    }

    /* grow */
    scache_t *oldcache = getcachefromsize(*start);
    scache_t *newcache = getcachefromsize(size);

    /* same allocation */
    if (oldcache == newcache) {
        uint64_t diff = size - currentsize;
        memset((void *)((uintptr_t)ptr + currentsize), 0, diff);
        *(start + 1) = size;
        return (void*)ptr;
    }

    /* different allocation */
    uint64_t *new = slab_allocate(newcache);
    if (new == NULL)
        return NULL;
    *(new + 1) = size;
    memcpy((new + 2), ptr, currentsize);
    slab_free(oldcache, start);
    return (void*)(new + 2);
}

void alloc_init()
{
    for (int i = 0; i < CACHE_COUNT; ++i) {
        caches[i] = slab_newcache(allocsizes[i] + sizeof(uint64_t) * 2
                                  + sizeof(uint64_t) * USE_POISON,
                                  0, initarea, dtor);
        ASSERT(caches[i]);
    }
    klogi("alloc: init SLAB-based memory allocator\n");
}
