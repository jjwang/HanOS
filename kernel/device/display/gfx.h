#pragma once

typedef struct {
    uint32_t id;

    volatile void* aperture_bar;
    volatile void* mmio_bar;
    volatile uint32_t* gtt_addr;
    uint16_t iobase;

    uint32_t aperture_size;
} gfx_pci_t;

typedef struct {
    uint32_t stolen_mem_size;
    uint32_t gtt_mem_size;
    uint32_t stolen_mem_base;

    uint32_t num_total_entries;     /* How many entries in the GTT */
    uint32_t num_mappable_entries;  /* How many can be mapped at once */

    volatile uint32_t* entries;
} gfx_gtt_t;

typedef struct {
    uint8_t *cpu_addr;
    volatile uint64_t gfx_addr;
} gfx_object_t;

typedef struct {
    uint64_t base;
    uint64_t top;
    uint64_t current;
} gfx_mem_range_t;

typedef struct {
    gfx_mem_range_t vram;   /* Stolen Memory */
    gfx_mem_range_t shared; /* Addresses mapped through aperture. */
    gfx_mem_range_t priv;   /* Only accessable by GPU, but allocated by CPU. */

    volatile uint8_t* gfx_mem_base;
    volatile uint8_t* gfx_mem_next;
} gfx_mem_manager_t;

#define FENCE_BASE                      0x100000
#define FENCE_COUNT                     16

void gfx_init(void);
pci_device_t pci_get_gfx_device(struct limine_kernel_address_response* kernel);

#define gfx_ind(pci, reg)           mmio_ind((uint8_t*)pci->mmio_bar + reg)
#define gfx_outd(pci, reg, val)     mmio_outd((uint8_t*)pci->mmio_bar + reg, val)
#define gfx_inl(pci, reg)           mmio_inl((uint8_t*)pci->mmio_bar + reg)
#define gfx_outl(pci, reg, val)     mmio_outl((uint8_t*)pci->mmio_bar + reg, val)

