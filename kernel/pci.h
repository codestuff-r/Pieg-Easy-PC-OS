#ifndef PIEG_PCI_H
#define PIEG_PCI_H

#include <stdint.h>

#define PCI_MAX_DEVICES 256

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t programming_interface;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint32_t bar[6];            /* raw BAR registers (type 0: 6, type 1: 2) */
} pci_device_t;

/* Scans the bus once and remembers every function found. */
void pci_init(void);

unsigned int pci_device_count(void);
const pci_device_t *pci_device_get(unsigned int index);

uint32_t pci_config_read32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);

#endif
