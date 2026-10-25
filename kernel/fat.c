#include "fat.h"
#include <stddef.h>

#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_LONG_NAME 0x0F
#define FAT_EOC 0x0FFFFFF8u

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static bool read_sector(blockdev_t *dev, uint32_t lba,
                        uint8_t buffer[512])
{
    return dev->sector_size == 512 &&
           blockdev_read(dev, lba, 1, buffer);
}

bool fat_mount(fat_fs_t *fs, blockdev_t *device)
{
    uint8_t b[512];

    if (!fs || !device || device->sector_size != 512)
        return false;

    if (!read_sector(device, 0, b))
        return false;

    if (b[510] != 0x55 || b[511] != 0xAA)
        return false;

    uint16_t bps = rd16(&b[11]);
    uint8_t spc = b[13];
    uint16_t reserved = rd16(&b[14]);
    uint8_t fats = b[16];
    uint16_t root_entries = rd16(&b[17]);
    uint32_t total_sectors = rd16(&b[19]);
    if (!total_sectors)
        total_sectors = rd32(&b[32]);

    uint32_t spf = rd16(&b[22]);
    if (!spf)
        spf = rd32(&b[36]);

    if (bps != 512 || !spc || (spc & (spc - 1)) ||
        !reserved || !fats || !spf || !total_sectors)
        return false;

    uint32_t root_sectors =
        ((uint32_t)root_entries * 32 + bps - 1) / bps;

    uint32_t first_data =
        (uint32_t)reserved + (uint32_t)fats * spf + root_sectors;

    if (first_data >= total_sectors)
        return false;

    uint32_t clusters = (total_sectors - first_data) / spc;

    fat_type_t type;
    if (clusters < 4085)
        type = FAT_TYPE_12;
    else if (clusters < 65525)
        type = FAT_TYPE_16;
    else
        type = FAT_TYPE_32;

    if (type == FAT_TYPE_32 && root_entries != 0)
        return false;

    if (type != FAT_TYPE_32 && root_entries == 0)
        return false;

    fs->device = device;
    fs->type = type;
    fs->bytes_per_sector = bps;
    fs->sectors_per_cluster = spc;
    fs->reserved_sectors = reserved;
    fs->fat_count = fats;
    fs->sectors_per_fat = spf;
    fs->root_cluster = type == FAT_TYPE_32
                     ? rd32(&b[44]) & 0x0FFFFFFF : 0;
    fs->root_dir_sectors = root_sectors;
    fs->first_fat_sector = reserved;
    fs->first_data_sector = first_data;
    fs->cluster_count = clusters;
    fs->root_dir_sector = reserved + (uint32_t)fats * spf;

    return true;
}

static bool next_cluster(fat_fs_t *fs, uint32_t cluster,
                         uint32_t *next)
{
    uint8_t b[512];
    uint32_t offset;
    uint32_t sector;

    if (fs->type == FAT_TYPE_12) {
        offset = cluster + cluster / 2;
        sector = fs->first_fat_sector + offset / 512;
        if (!read_sector(fs->device, sector, b))
            return false;

        uint16_t value = rd16(&b[offset % 512]);
        *next = (cluster & 1) ? (value >> 4) : (value & 0x0FFF);
        return true;
    }

    if (fs->type == FAT_TYPE_16) {
        offset = cluster * 2;
        sector = fs->first_fat_sector + offset / 512;
        if (!read_sector(fs->device, sector, b))
            return false;

        *next = rd16(&b[offset % 512]);
        return true;
    }

    offset = cluster * 4;
    sector = fs->first_fat_sector + offset / 512;
    if (!read_sector(fs->device, sector, b))
        return false;

    *next = rd32(&b[offset % 512]) & 0x0FFFFFFF;
    return true;
}

static void format_name(const uint8_t *entry, char out[13])
{
    unsigned int p = 0;
    unsigned int end = 8;

    while (end && entry[end - 1] == ' ')
        end--;

    for (unsigned int i = 0; i < end; i++)
        out[p++] = (char)entry[i];

    end = 3;
    while (end && entry[8 + end - 1] == ' ')
        end--;

    if (end) {
        out[p++] = '.';
        for (unsigned int i = 0; i < end; i++)
            out[p++] = (char)entry[8 + i];
    }

    out[p] = '\0';
}

static bool scan_sector(fat_fs_t *fs, uint32_t sector,
                        fat_dir_callback_t callback, void *context,
                        bool *finished)
{
    uint8_t b[512];

    if (!read_sector(fs->device, sector, b))
        return false;

    for (unsigned int i = 0; i < 512; i += 32) {
        uint8_t *e = &b[i];

        if (e[0] == 0x00) {
            *finished = true;
            return true;
        }

        if (e[0] == 0xE5 || e[11] == FAT_ATTR_LONG_NAME ||
            (e[11] & FAT_ATTR_VOLUME_ID))
            continue;

        char name[13];
        format_name(e, name);

        uint32_t size = rd32(&e[28]);
        callback(name, e[11], size, context);
    }

    return true;
}

bool fat_list_root(fat_fs_t *fs,
                   fat_dir_callback_t callback,
                   void *context)
{
    bool finished = false;

    if (!fs || !fs->device || !callback)
        return false;

    if (fs->type != FAT_TYPE_32) {
        for (uint32_t i = 0; i < fs->root_dir_sectors; i++) {
            if (!scan_sector(fs, fs->root_dir_sector + i,
                             callback, context, &finished))
                return false;
            if (finished)
                return true;
        }
        return true;
    }

    uint32_t cluster = fs->root_cluster;
    uint32_t limit = fs->cluster_count;

    while (limit-- && cluster >= 2 &&
           cluster < fs->cluster_count + 2) {
        uint32_t first = fs->first_data_sector
                       + (cluster - 2) * fs->sectors_per_cluster;

        for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
            if (!scan_sector(fs, first + i, callback, context,
                             &finished))
                return false;
            if (finished)
                return true;
        }

        uint32_t next;
        if (!next_cluster(fs, cluster, &next))
            return false;
        if (next >= FAT_EOC)
            return true;
        if (next < 2)
            return false;

        cluster = next;
    }

    return limit != 0;
}
