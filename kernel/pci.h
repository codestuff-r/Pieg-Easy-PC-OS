#ifndef PIEG_PCI_H
#define PIEG_PCI_H

#include <stdint.h>

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
} pci_device_t;

void pci_init(void);
uint32_t pci_config_read32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset);

#endif
