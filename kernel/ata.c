
#include <stdint.h>
#include "ata.h"

extern void vga_write(const char *text);

#define ATA_DATA       0x1F0
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA0       0x1F3
#define ATA_LBA1       0x1F4
#define ATA_LBA2       0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALTSTATUS  0x3F6

#define ATA_BSY  0x80
#define ATA_DF   0x20
#define ATA_ERR  0x01
#define ATA_DRQ  0x08

#define ATA_IDENTIFY 0xEC
#define ATA_READ_PIO 0x20
#define ATA_WRITE_PIO 0x30
#define ATA_CACHE_FLUSH 0xE7

static uint32_t disk_sectors;

static inline void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static inline void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" :: "a"(value), "Nd"(port));
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void io_wait(void)
{
    for (unsigned int i = 0; i < 4; i++)
        (void)inb(ATA_ALTSTATUS);
}

static int wait_ready(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return -1;

        if (!(status & ATA_BSY))
            return 0;
    }

    return -1;
}

static int wait_data(void)
{
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status == 0 || status == 0xFF)
            return -1;

        if (status & ATA_BSY)
            continue;

        if (status & (ATA_ERR | ATA_DF))
            return -1;

        if (status & ATA_DRQ)
            return 0;
    }

    return -1;
}

static void print_hex8(uint8_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    char text[3];

    text[0] = hex[value >> 4];
    text[1] = hex[value & 0x0F];
    text[2] = '\0';

    vga_write(text);
}

static void print_u32(uint32_t value)
{
    char text[10];
    unsigned int n = 0;

    if (value == 0) {
        vga_write("0");
        return;
    }

    while (value && n < sizeof(text)) {
        text[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n)
        vga_write((char[2]){text[--n], '\0'});
}

int ata_read_sector(uint32_t lba, uint8_t *buffer)
{
    if (!buffer || lba >= disk_sectors || lba >= 0x10000000u)
        return -1;

    if (wait_ready() != 0)
        return -1;

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    io_wait();

    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_READ_PIO);

    io_wait();

    if (wait_data() != 0)
        return -1;

    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = inw(ATA_DATA);
        buffer[i * 2] = (uint8_t)word;
        buffer[i * 2 + 1] = (uint8_t)(word >> 8);
    }

    io_wait();
    return 0;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer)
{
    if (!buffer || lba >= disk_sectors || lba >= 0x10000000u)
        return -1;

    if (wait_ready() != 0)
        return -1;

    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    io_wait();

    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA0, (uint8_t)lba);
    outb(ATA_LBA1, (uint8_t)(lba >> 8));
    outb(ATA_LBA2, (uint8_t)(lba >> 16));
    outb(ATA_COMMAND, ATA_WRITE_PIO);

    io_wait();

    if (wait_data() != 0)
        return -1;

    for (unsigned int i = 0; i < 256; i++) {
        uint16_t word = (uint16_t)buffer[i * 2]
                      | ((uint16_t)buffer[i * 2 + 1] << 8);
        outw(ATA_DATA, word);
    }

    outb(ATA_COMMAND, ATA_CACHE_FLUSH);
    io_wait();

    return wait_ready();
}

uint32_t ata_sector_count(void)
{
    return disk_sectors;
}

int ata_init(void)
{
    uint16_t identify[256];

    disk_sectors = 0;
    vga_write("\n[ATA] Detecting primary-master IDE disk...\n");

    outb(ATA_DRIVE, 0xA0);
    io_wait();

    outb(ATA_SECCOUNT, 0);
    outb(ATA_LBA0, 0);
    outb(ATA_LBA1, 0);
    outb(ATA_LBA2, 0);
    outb(ATA_COMMAND, ATA_IDENTIFY);

    io_wait();

    uint8_t status = inb(ATA_STATUS);

    if (status == 0 || status == 0xFF) {
        vga_write("[ATA] No disk on primary master.\n");
        return -1;
    }

    if (wait_data() != 0) {
        vga_write("[ATA] IDENTIFY failed or timed out.\n");
        return -1;
    }

    for (unsigned int i = 0; i < 256; i++)
        identify[i] = inw(ATA_DATA);

    disk_sectors = (uint32_t)identify[60]
                 | ((uint32_t)identify[61] << 16);

    if (disk_sectors == 0) {
        vga_write("[ATA] Disk reported zero sectors.\n");
        return -1;
    }

    vga_write("[ATA] Disk detected. Sectors: ");
    print_u32(disk_sectors);
    vga_write("\n");

    uint8_t sector[512];

    if (ata_read_sector(0, sector) != 0) {
        vga_write("[ATA] Could not read sector 0.\n");
        return -1;
    }

    vga_write("[ATA] Sector 0 read successfully. Signature: 0x");
    print_hex8(sector[511]);
    print_hex8(sector[510]);
    vga_write("\n");

    return 0;
}
