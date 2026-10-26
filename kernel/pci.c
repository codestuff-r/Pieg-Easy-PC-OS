#include <stdint.h>
#include "pci.h"

extern void vga_write(const char *text);

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static pci_device_t devices[PCI_MAX_DEVICES];
static unsigned int device_count;

static inline void outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset)
{
    uint32_t address =
        0x80000000u |
        ((uint32_t)bus << 16) |
        ((uint32_t)device << 11) |
        ((uint32_t)function << 8) |
        (offset & 0xFC);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device,
                           uint8_t function, uint8_t offset)
{
    uint32_t value = pci_config_read32(bus, device, function, offset);
    return (uint16_t)(value >> ((offset & 2) * 8));
}

unsigned int pci_device_count(void)
{
    return device_count;
}

const pci_device_t *pci_device_get(unsigned int index)
{
    return index < device_count ? &devices[index] : 0;
}

static void print_u32(unsigned int value)
{
    char text[11];
    unsigned int n = 0;

    if (value == 0) {
        vga_write("0");
        return;
    }

    while (value && n < sizeof(text) - 1) {
        text[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    char out[11];
    unsigned int i = 0;

    while (n)
        out[i++] = text[--n];

    out[i] = '\0';
    vga_write(out);
}

static void record(uint8_t bus, uint8_t device, uint8_t function)
{
    if (device_count >= PCI_MAX_DEVICES)
        return;

    pci_device_t *d = &devices[device_count];
    uint32_t id = pci_config_read32(bus, device, function, 0x00);
    uint32_t classreg = pci_config_read32(bus, device, function, 0x08);
    uint32_t header = pci_config_read32(bus, device, function, 0x0C);

    d->bus = bus;
    d->device = device;
    d->function = function;
    d->vendor_id = (uint16_t)id;
    d->device_id = (uint16_t)(id >> 16);
    d->revision = (uint8_t)classreg;
    d->programming_interface = (uint8_t)(classreg >> 8);
    d->subclass = (uint8_t)(classreg >> 16);
    d->class_code = (uint8_t)(classreg >> 24);
    d->header_type = (uint8_t)((header >> 16) & 0x7F);
    d->irq_line = (uint8_t)pci_config_read32(bus, device, function, 0x3C);

    for (unsigned int i = 0; i < 6; i++)
        d->bar[i] = 0;

    /* Type 0 (endpoint) has 6 BARs, type 1 (bridge) has 2, others none. */
    unsigned int bars = d->header_type == 0 ? 6 : d->header_type == 1 ? 2 : 0;

    for (unsigned int i = 0; i < bars; i++)
        d->bar[i] = pci_config_read32(bus, device, function,
                                      (uint8_t)(0x10 + i * 4));

    device_count++;
}

void pci_init(void)
{
    device_count = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint32_t id0 = pci_config_read32((uint8_t)bus, device, 0, 0);

            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;

            uint8_t header = (uint8_t)(
                pci_config_read32((uint8_t)bus, device, 0, 0x0C) >> 16);
            uint8_t functions = (header & 0x80) ? 8 : 1;

            for (uint8_t function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32((uint8_t)bus, device,
                                                function, 0);

                if ((id & 0xFFFF) == 0xFFFF)
                    continue;

                record((uint8_t)bus, device, function);
            }
        }
    }

    vga_write("PCI: ");
    print_u32(device_count);
    vga_write(" functions found (run lspci)\n");
}
