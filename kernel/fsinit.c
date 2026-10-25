
#include <stdint.h>
#include <stdbool.h>

#include "ata.h"
#include "blockdev.h"
#include "fat.h"
#include "fsinit.h"

extern void vga_write(const char *text);
extern char keyboard_getchar(void);

static blockdev_t ata_device;
static fat_fs_t root_fs;
static bool device_registered;
static bool filesystem_mounted;

static bool ata_block_read(
    blockdev_t *dev,
    uint64_t lba,
    uint32_t count,
    void *buffer
)
{
    (void)dev;

    if (!buffer || count == 0 || lba >= ata_sector_count())
        return false;

    if (count > ata_sector_count() - lba)
        return false;

    uint8_t *out = (uint8_t *)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_sector((uint32_t)(lba + i), out) != 0)
            return false;

        out += BLOCKDEV_SECTOR_SIZE;
    }

    return true;
}

void fsinit(void)
{
    filesystem_mounted = false;
    device_registered = false;

    vga_write("\n[FS] Filesystem initialization\n");
    vga_write("Mount FAT filesystem? [Y/N]: ");

    char answer;

    for (;;) {
        answer = keyboard_getchar();

        if (answer == 'y' || answer == 'Y')
            break;

        if (answer == 'n' || answer == 'N') {
            vga_write("\n[FS] Mount skipped.\n");
            vga_write("Don't worry, the core system still works!\n");
            return;
        }
    }

    vga_write("\n[FS] Initializing filesystem...\n");

    uint32_t sectors = ata_sector_count();

    if (sectors == 0) {
        vga_write("[FS] No ATA disk detected.\n");
        vga_write("Don't worry, the core system still works!\n");
        return;
    }

    ata_device.name = "hd0";
    ata_device.sector_count = sectors;
    ata_device.sector_size = BLOCKDEV_SECTOR_SIZE;
    ata_device.driver_data = 0;
    ata_device.read = ata_block_read;
    ata_device.write = 0;

    if (!blockdev_register(&ata_device)) {
        vga_write("[FS] Block-device registration failed.\n");
        vga_write("Don't worry, the core system still works!\n");
        return;
    }

    device_registered = true;
    vga_write("[FS] Block device registered.\n");
    vga_write("[FS] Mounting FAT filesystem...\n");

    if (!fat_mount(&root_fs, &ata_device)) {
        vga_write("[FS] FAT mount failed.\n");
        vga_write("Don't worry, the core system still works!\n");
        return;
    }

    filesystem_mounted = true;
    vga_write("[FS] FAT volume mounted successfully.\n");
}

bool fs_is_mounted(void)
{
    return filesystem_mounted;
}

uint64_t fs_disk_sectors(void)
{
    return device_registered ? ata_device.sector_count : 0;
}

const char *fs_type_name(void)
{
    if (!filesystem_mounted)
        return "none";

    switch (root_fs.type) {
    case FAT_TYPE_12: return "FAT12";
    case FAT_TYPE_16: return "FAT16";
    case FAT_TYPE_32: return "FAT32";
    default: return "unknown";
    }
}

bool fs_list_root(
    void (*callback)(
        const char *name,
        uint8_t attributes,
        uint32_t size,
        void *context
    ),
    void *context
)
{
    if (!filesystem_mounted || !callback)
        return false;

    return fat_list_root(&root_fs, callback, context);
}
