#include <stdint.h>
#include "pci.h"

extern void vga_write(const char *text);

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

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

static void print_hex16(uint16_t value)
{
    static const char digits[] = "0123456789ABCDEF";
    char output[5] = {
        digits[(value >> 12) & 0xF],
        digits[(value >> 8) & 0xF],
        digits[(value >> 4) & 0xF],
        digits[value & 0xF],
        '\0'
    };

    vga_write(output);
}

void pci_init(void)
{
    unsigned int found = 0;

    vga_write("\nPCI: scanning hardware...\n");

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint32_t id0 = pci_config_read32(
                (uint8_t)bus, device, 0, 0
            );

            if ((id0 & 0xFFFF) == 0xFFFF)
                continue;

            uint8_t header = (uint8_t)(
                pci_config_read32((uint8_t)bus, device, 0, 0x0C) >> 16
            );

            uint8_t functions = (header & 0x80) ? 8 : 1;

            for (uint8_t function = 0; function < functions; function++) {
                uint32_t id = pci_config_read32(
                    (uint8_t)bus, device, function, 0
                );

                uint16_t vendor = (uint16_t)id;

                if (vendor == 0xFFFF)
                    continue;

                uint16_t product = (uint16_t)(id >> 16);
                uint32_t classreg = pci_config_read32(
                    (uint8_t)bus, device, function, 0x08
                );

                uint8_t revision = (uint8_t)classreg;
                uint8_t prog_if = (uint8_t)(classreg >> 8);
                uint8_t subclass = (uint8_t)(classreg >> 16);
                uint8_t class_code = (uint8_t)(classreg >> 24);

                vga_write("PCI ");
                print_hex16((uint16_t)bus);
                vga_write(":");
                print_hex16(device);
                vga_write(".");
                print_hex16(function);
                vga_write(" vendor=");
                print_hex16(vendor);
                vga_write(" device=");
                print_hex16(product);
                vga_write(" class=");
                print_hex16(((uint16_t)class_code << 8) | subclass);
                vga_write(" prog-if=");
                print_hex16(prog_if);
                vga_write("\n");

                (void)revision;
                found++;
            }
        }
    }

    vga_write("PCI scan complete. Devices found: ");
    if (found == 0)
        vga_write("0\n");
    else
        vga_write("see device list above\n");
}
