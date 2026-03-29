/**-----------------------------------------------------------------------------

 @file    slab.h
 @brief   Slab allocator definitions
 @details
 @verbatim

  This file contains the type definitions and function declarations for the
  slab allocator used in the HanOS kernel. It defines structures for representing
  slabs and slab caches, and provides function prototypes for allocating and
  freeing memory from these caches. Additionally, it includes functions for
  creating and destroying slab caches, each with customizable constructors and
  destructors for initializing and cleaning up objects.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <base/spinlock.h>

typedef struct slab_t {
    struct slab_t *next;
    struct slab_t *prev;
    uint64_t used;
    void **free;
    void *base;
} slab_t;

typedef struct scache_t {
    lock_t lock;
    void (*ctor)(struct scache_t * cache, void *obj);
    void (*dtor)(struct scache_t * cache, void *obj);
    slab_t *full;
    slab_t *partial;
    slab_t *empty;
    uint64_t size;
    uint64_t truesize;
    uint64_t alignment;
    uint64_t slabobjcount;
} scache_t;

void *slab_allocate(scache_t * cache);

void slab_free(scache_t * cache, void *addr);

scache_t *slab_newcache(uint64_t size, uint64_t alignment,
                        void (*ctor)(scache_t *, void *),
                        void(*dtor)(scache_t *, void *));

void slab_freecache(scache_t * cache);
