#ifndef PIEG_FAT_H
#define PIEG_FAT_H

#include "blockdev.h"
#include <stdbool.h>
#include <stdint.h>

#define FAT_ATTR_READONLY  0x01
#define FAT_ATTR_HIDDEN    0x02
#define FAT_ATTR_SYSTEM    0x04
#define FAT_ATTR_VOLUME_ID 0x08
#define FAT_ATTR_DIRECTORY 0x10
#define FAT_ATTR_ARCHIVE   0x20
#define FAT_ATTR_LONG_NAME 0x0F

#define FAT_NAME_MAX 255
#define FAT_PATH_MAX 256

typedef enum {
    FAT_OK = 0,
    FAT_ERR_IO,
    FAT_ERR_NOFS,
    FAT_ERR_NOTFOUND,
    FAT_ERR_EXISTS,
    FAT_ERR_NOTDIR,
    FAT_ERR_ISDIR,
    FAT_ERR_NOSPACE,
    FAT_ERR_NAME,
    FAT_ERR_NOTEMPTY,
    FAT_ERR_READONLY,
    FAT_ERR_INVALID
} fat_err_t;

typedef enum {
    FAT_TYPE_12 = 12,
    FAT_TYPE_16 = 16,
    FAT_TYPE_32 = 32
} fat_type_t;

typedef struct {
    blockdev_t *device;
    fat_type_t type;
    bool writable;
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
    uint32_t next_free;
    char label[12];

    /* One-sector write-through cache for the FAT. */
    uint8_t fc_buf[512];
    uint32_t fc_lba;
    bool fc_valid;
} fat_fs_t;

typedef struct {
    char name[FAT_NAME_MAX + 1];
    char short_name[13];
    uint8_t attr;
    uint32_t size;
    uint32_t cluster;       /* first cluster, 0 = none (root on FAT12/16) */
    uint32_t dir_cluster;   /* directory holding this entry */
    uint32_t entry_lba;     /* sector of the 8.3 entry, 0 = synthetic root */
    uint16_t entry_off;
    uint8_t lfn_count;
    uint32_t lfn_lba[20];
    uint16_t lfn_off[20];
} fat_dirent_t;

typedef void (*fat_dir_callback_t)(
    const char *name,
    uint8_t attributes,
    uint32_t size,
    void *context
);

bool fat_mount(fat_fs_t *fs, blockdev_t *device);

/* Paths are absolute and normalised: "/", "/U/HOME/FILE.TXT". */
fat_err_t fat_lookup(fat_fs_t *fs, const char *path, fat_dirent_t *out);
fat_err_t fat_list(fat_fs_t *fs, const char *path,
                   fat_dir_callback_t callback, void *context);
fat_err_t fat_read(fat_fs_t *fs, const char *path, uint32_t offset,
                   void *buffer, uint32_t length, uint32_t *done);
fat_err_t fat_write(fat_fs_t *fs, const char *path, const void *data,
                    uint32_t length, bool append);
fat_err_t fat_create(fat_fs_t *fs, const char *path);
fat_err_t fat_mkdir(fat_fs_t *fs, const char *path);
fat_err_t fat_remove(fat_fs_t *fs, const char *path);
fat_err_t fat_stats(fat_fs_t *fs, uint32_t *total_clusters,
                    uint32_t *free_clusters);

const char *fat_error_string(fat_err_t error);

#endif
