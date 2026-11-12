#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint16_t device_id;
    bool gen9;
    bool pte64;

    uint32_t gtt_offset;
    uint32_t gtt_size;

    volatile void *aperture_bar;
    volatile void *mmio_bar;
    volatile uint64_t *gtt_addr;

    uint32_t aperture_size;
} gfx_pci_t;

typedef struct {
    uint32_t stolen_mem_size;
    uint32_t gtt_mem_size;
    uint32_t stolen_mem_base;

    uint32_t num_total_entries;
    uint32_t num_mappable_entries;
    bool pte64;

    volatile uint64_t *entries;
} gfx_gtt_t;

typedef struct {
    volatile uint8_t *cpu_addr;
    volatile uint64_t gfx_addr;
    uint64_t phys;
    uint64_t size;
    uint32_t pages;
    int64_t mem_handle;
} gfx_object_t;

typedef struct {
    uint64_t base;
    uint64_t top;
    uint64_t current;
} gfx_mem_range_t;

typedef struct {
    gfx_mem_range_t vram;
    gfx_mem_range_t shared;
    gfx_mem_range_t priv;

    volatile uint8_t *gfx_mem_base;
    volatile uint8_t *gfx_mem_next;
} gfx_mem_manager_t;

typedef struct {
    gfx_object_t obj;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t *pages;
    uint32_t page_count;
    int64_t mem_handle;
} gfx_fb_t;

#define FENCE_BASE      0x100000
#define FENCE_COUNT     16
