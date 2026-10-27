#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ahci.h"
#include "ata.h"
#include "blockdev.h"
#include "fat.h"
#include "fsinit.h"
#include "mem.h"
#include "partition.h"
#include "ramdisk.h"

extern void vga_write(const char *text);

static blockdev_t ata_device;
static partition_t partitions[PARTITION_MAX];
static unsigned int partition_count;

#define FS_DISK_MAX 8

static blockdev_t *disk_list[FS_DISK_MAX];
static unsigned int disk_total;

static fat_fs_t root_fs;
static bool filesystem_mounted;
static bool pieg_volume;

/* ================================================================
 * ATA block device
 * ================================================================ */

static bool ata_block_read(blockdev_t *dev, uint64_t lba, uint32_t count,
                           void *buffer)
{
    (void)dev;

    uint8_t *out = (uint8_t *)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (ata_read_sector((uint32_t)(lba + i), out) != 0)
            return false;

        out += BLOCKDEV_SECTOR_SIZE;
    }

    return true;
}

static bool ata_block_write(blockdev_t *dev, uint64_t lba, uint32_t count,
                            const void *buffer)
{
    (void)dev;

    const uint8_t *in = (const uint8_t *)buffer;

    for (uint32_t i = 0; i < count; i++) {
        if (ata_write_sector((uint32_t)(lba + i), in) != 0)
            return false;

        in += BLOCKDEV_SECTOR_SIZE;
    }

    return true;
}

/* ================================================================
 * Printing
 * ================================================================ */

static void print_u32(uint32_t value)
{
    char text[11];
    unsigned int n = 0;

    if (value == 0) {
        vga_write("0");
        return;
    }

    while (value) {
        text[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n) {
        char one[2] = { text[--n], '\0' };
        vga_write(one);
    }
}

static const char *type_name(fat_type_t type)
{
    switch (type) {
    case FAT_TYPE_12: return "FAT12";
    case FAT_TYPE_16: return "FAT16";
    case FAT_TYPE_32: return "FAT32";
    }

    return "unknown";
}

/* ================================================================
 * Volume discovery
 * ================================================================ */

static bool label_is_pieg(const char *label)
{
    const char *want = PIEG_VOLUME_LABEL;

    while (*want) {
        char c = *label++;

        if (c >= 'a' && c <= 'z')
            c = (char)(c - 32);

        if (c != *want++)
            return false;
    }

    return *label == '\0';
}

/* Mounts `dev` into `fs`; *pieg is true for a PIEG system volume. */
static bool probe(blockdev_t *dev, fat_fs_t *fs, bool *pieg)
{
    fat_dirent_t entry;

    if (!fat_mount(fs, dev))
        return false;

    *pieg = label_is_pieg(fs->label) ||
            fat_lookup(fs, PIEG_DIR_SYSTEM, &entry) == FAT_OK;

    return true;
}

static void ensure_dir(const char *path)
{
    fat_dirent_t entry;

    if (fat_lookup(&root_fs, path, &entry) == FAT_OK)
        return;

    if (fat_mkdir(&root_fs, path) != FAT_OK) {
        vga_write("[FS] Could not create ");
        vga_write(path);
        vga_write("\n");
    }
}

static void ensure_layout(void)
{
    if (!root_fs.writable)
        return;

    ensure_dir(PIEG_DIR_SYSTEM);
    ensure_dir(PIEG_DIR_PROGRAMS);
    ensure_dir(PIEG_DIR_USERS);
    ensure_dir(PIEG_DIR_HOME);
}

/* Registers a whole disk, then every partition found on it. */
static void add_disk(blockdev_t *dev)
{
    if (!blockdev_register(dev))
        return;

    if (disk_total < FS_DISK_MAX)
        disk_list[disk_total++] = dev;

    unsigned int first = partition_count;

    partition_count += partition_scan(dev, &partitions[partition_count],
                                      PARTITION_MAX - partition_count);

    for (unsigned int i = first; i < partition_count; i++) {
        blockdev_register(&partitions[i].dev);

        vga_write("[FS] ");
        vga_write(partitions[i].name);
        vga_write(": partition, ");
        print_u32((uint32_t)(partitions[i].dev.sector_count / 2048));
        vga_write(" MiB\n");
    }
}

void fsinit(uint32_t magic, uint32_t mbi)
{
    static fat_fs_t scratch;

    filesystem_mounted = false;
    pieg_volume = false;

    vga_write("\n[FS] Storage initialization\n");

    ramdisk_init(magic, mbi);

    if (ata_sector_count() != 0) {
        ata_device.name = "hd0";
        ata_device.sector_count = ata_sector_count();
        ata_device.sector_size = BLOCKDEV_SECTOR_SIZE;
        ata_device.driver_data = 0;
        ata_device.read = ata_block_read;
        ata_device.write = ata_block_write;

        add_disk(&ata_device);
    }

    for (unsigned int i = 0; i < ahci_disk_count(); i++)
        add_disk(ahci_disk_get(i));

    /*
     * Registration order is ramdisk, disk, partitions. Scanning backwards
     * prefers a real partition over the whole disk over the live-medium
     * ramdisk, so a persistent PIEG disk wins over the volatile live image.
     * Any PIEG volume wins over a foreign FAT volume.
     */
    blockdev_t *pieg_dev = 0;
    blockdev_t *foreign_dev = 0;

    for (unsigned int i = blockdev_count(); i-- > 0;) {
        blockdev_t *dev = blockdev_get(i);
        bool pieg;

        if (!probe(dev, &scratch, &pieg))
            continue;

        vga_write("[FS] ");
        vga_write(dev->name);
        vga_write(": ");
        vga_write(type_name(scratch.type));
        vga_write(" label=");
        vga_write(scratch.label[0] ? scratch.label : "(none)");
        vga_write(pieg ? " [PIEG volume]\n" : "\n");

        if (pieg && !pieg_dev)
            pieg_dev = dev;
        else if (!pieg && !foreign_dev)
            foreign_dev = dev;
    }

    blockdev_t *chosen = pieg_dev ? pieg_dev : foreign_dev;

    if (!chosen) {
        vga_write("[FS] No FAT volume found.\n");
        vga_write("Don't worry, the core system still works!\n");
        return;
    }

    if (!fat_mount(&root_fs, chosen)) {
        vga_write("[FS] FAT mount failed.\n");
        vga_write("Don't worry, the core system still works!\n");
        return;
    }

    filesystem_mounted = true;
    pieg_volume = pieg_dev != 0;

    if (pieg_volume)
        ensure_layout();

    vga_write("[FS] Mounted ");
    vga_write(chosen->name);
    vga_write(" as /\n");
}

/* ================================================================
 * Queries
 * ================================================================ */

unsigned int fs_disk_count(void)
{
    return disk_total;
}

blockdev_t *fs_disk_get(unsigned int index)
{
    return index < disk_total ? disk_list[index] : 0;
}

fat_fs_t *fs_root(void)
{
    return filesystem_mounted ? &root_fs : 0;
}

bool fs_is_mounted(void)
{
    return filesystem_mounted;
}

bool fs_is_writable(void)
{
    return filesystem_mounted && root_fs.writable;
}

bool fs_is_pieg_volume(void)
{
    return filesystem_mounted && pieg_volume;
}

uint64_t fs_disk_sectors(void)
{
    return filesystem_mounted ? root_fs.device->sector_count : 0;
}

const char *fs_type_name(void)
{
    if (!filesystem_mounted)
        return "none";

    return type_name(root_fs.type);
}

const char *fs_device_name(void)
{
    return filesystem_mounted ? root_fs.device->name : "none";
}

const char *fs_label(void)
{
    return filesystem_mounted ? root_fs.label : "";
}

/* ================================================================
 * File operations
 * ================================================================ */

fat_err_t fs_list(const char *path, fs_list_callback_t callback,
                  void *context)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_list(&root_fs, path, callback, context);
}

fat_err_t fs_stat(const char *path, bool *is_directory, uint32_t *size)
{
    fat_dirent_t entry;

    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    fat_err_t r = fat_lookup(&root_fs, path, &entry);

    if (r != FAT_OK)
        return r;

    if (is_directory)
        *is_directory = (entry.attr & FAT_ATTR_DIRECTORY) != 0;
    if (size)
        *size = entry.size;

    return FAT_OK;
}

fat_err_t fs_read(const char *path, uint32_t offset, void *buffer,
                  uint32_t length, uint32_t *done)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_read(&root_fs, path, offset, buffer, length, done);
}

fat_err_t fs_write(const char *path, const void *data, uint32_t length,
                   bool append)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_write(&root_fs, path, data, length, append);
}

fat_err_t fs_create(const char *path)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_create(&root_fs, path);
}

fat_err_t fs_mkdir(const char *path)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_mkdir(&root_fs, path);
}

fat_err_t fs_remove(const char *path)
{
    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    return fat_remove(&root_fs, path);
}

fat_err_t fs_df(uint64_t *total_bytes, uint64_t *free_bytes)
{
    uint32_t total;
    uint32_t free_clusters;

    if (!filesystem_mounted)
        return FAT_ERR_NOFS;

    fat_err_t r = fat_stats(&root_fs, &total, &free_clusters);

    if (r != FAT_OK)
        return r;

    uint64_t cluster_bytes = (uint64_t)root_fs.sectors_per_cluster * 512u;

    if (total_bytes)
        *total_bytes = (uint64_t)total * cluster_bytes;
    if (free_bytes)
        *free_bytes = (uint64_t)free_clusters * cluster_bytes;

    return FAT_OK;
}

/* ================================================================
 * Path resolution
 * ================================================================ */

void fs_resolve(const char *cwd, const char *arg, char *out, size_t capacity)
{
    char joined[2 * FAT_PATH_MAX + 2];
    size_t n = 0;

    if (!capacity)
        return;

    if (!arg)
        arg = "";

    if (arg[0] == '~' && (arg[1] == '\0' || arg[1] == '/')) {
        const char *home = PIEG_DIR_HOME;

        while (*home && n < sizeof(joined) - 1)
            joined[n++] = *home++;

        arg++;
    } else if (arg[0] != '/') {
        const char *base = cwd ? cwd : "/";

        while (*base && n < sizeof(joined) - 1)
            joined[n++] = *base++;

        if (n < sizeof(joined) - 1)
            joined[n++] = '/';
    }

    while (*arg && n < sizeof(joined) - 1)
        joined[n++] = *arg++;

    joined[n] = '\0';

    size_t len = 1;

    if (capacity < 2) {
        out[0] = '\0';
        return;
    }

    out[0] = '/';
    out[1] = '\0';

    const char *p = joined;

    for (;;) {
        while (*p == '/')
            p++;

        if (!*p)
            break;

        const char *start = p;

        while (*p && *p != '/')
            p++;

        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.')
            continue;

        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 1 && out[len - 1] != '/')
                len--;

            if (len > 1)
                len--;

            out[len] = '\0';
            continue;
        }

        if (len + (len > 1 ? 1 : 0) + clen + 1 > capacity)
            break;

        if (len > 1)
            out[len++] = '/';

        for (size_t i = 0; i < clen; i++)
            out[len++] = start[i];

        out[len] = '\0';
    }
}
