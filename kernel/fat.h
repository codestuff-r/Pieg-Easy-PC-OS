#ifndef PIEG_FAT_H
#define PIEG_FAT_H

#include "blockdev.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32
} fat_type_t;

typedef struct {
    blockdev_t *device;
    fat_type_t type;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t root_cluster;
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t first_data_sector;
    uint32_t cluster_count;
    uint32_t root_dir_sector;
} fat_fs_t;

typedef void (*fat_dir_callback_t)(
    const char *name,
    uint8_t attributes,
    uint32_t size,
    void *context
);

bool fat_mount(fat_fs_t *fs, blockdev_t *device);
bool fat_list_root(fat_fs_t *fs,
                   fat_dir_callback_t callback,
                   void *context);

#endif
