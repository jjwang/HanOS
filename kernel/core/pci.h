#pragma once

#include <stdint.h>
#include <stddef.h>

typedef struct [[gnu::packed]] {
    int64_t parent;
    uint8_t bus;
    uint8_t func;
    uint8_t device;
    uint16_t device_id;
    uint16_t vendor_id;
    uint8_t rev_id;
    uint8_t subclass;
    uint8_t device_class;
    uint8_t prog_if;
    int multifunction;
    uint8_t irq_pin;
    int has_prt;
    uint32_t gsi;
    uint16_t gsi_flags;
} pci_device_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t device_id;
    char desc[256];
} pci_device_desc_t;

void pci_init(void);
void pci_debug(void);

