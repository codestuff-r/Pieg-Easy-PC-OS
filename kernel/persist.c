#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "blockdev.h"
#include "fat.h"
#include "fsinit.h"
#include "gpt.h"
#include "mem.h"
#include "mkfat.h"
#include "partition.h"
#include "persist.h"

extern void vga_write(const char *text);

#define DEFAULT_MAX_SECTORS (4ull * 1024 * 1024 * 2)   /* 4 GiB */
#define COPY_CHUNK          4096u
#define COPY_MAX_ITEMS      48u
#define COPY_MAX_DEPTH      4u

/* ================================================================
 * Disk listing
 * ================================================================ */

unsigned int persist_disk_count(void)
{
    return fs_disk_count();
}

int persist_disk_info(unsigned int index, persist_disk_t *info)
{
    blockdev_t *disk = fs_disk_get(index);

    if (!disk || !info)
        return 0;

    memset(info, 0, sizeof(*info));

    for (unsigned int i = 0; i + 1 < sizeof(info->name) && disk->name[i]; i++)
        info->name[i] = disk->name[i];

    info->sectors = disk->sector_count;
    info->state = (int)gpt_inspect(disk, &info->free_sectors);

    return 1;
}

/* ================================================================
 * Copying the running system onto the new volume
 * ================================================================ */

typedef struct {
    char name[16];
    uint8_t attr;
} item_t;

typedef struct {
    item_t items[COPY_MAX_ITEMS];
    unsigned int count;
} items_t;

static fat_fs_t new_fs;
static partition_t new_part;
static uint8_t copy_buf[COPY_CHUNK];
static unsigned int copy_failures;

static void collect(const char *name, uint8_t attributes, uint32_t size,
                    void *context)
{
    items_t *list = (items_t *)context;
    (void)size;

    if (name[0] == '.' || (attributes & FAT_ATTR_VOLUME_ID))
        return;

    size_t n = 0;

    while (name[n])
        n++;

    if (n == 0 || n >= sizeof(list->items[0].name) ||
        list->count >= COPY_MAX_ITEMS) {
        copy_failures++;            /* long name or too many entries */
        return;
    }

    for (size_t i = 0; i <= n; i++)
        list->items[list->count].name[i] = name[i];

    list->items[list->count].attr = attributes;
    list->count++;
}

static bool join(char *out, size_t capacity, const char *dir, const char *name)
{
    size_t n = 0;

    while (dir[n] && n + 1 < capacity) {
        out[n] = dir[n];
        n++;
    }

    if (n == 0 || out[n - 1] != '/') {
        if (n + 1 >= capacity)
            return false;

        out[n++] = '/';
    }

    for (size_t i = 0; name[i]; i++) {
        if (n + 1 >= capacity)
            return false;

        out[n++] = name[i];
    }

    out[n] = '\0';
    return true;
}

static void copy_file(fat_fs_t *src, const char *spath, const char *dpath)
{
    uint32_t offset = 0;
    uint32_t done = 0;

    if (fat_create(&new_fs, dpath) != FAT_OK) {
        copy_failures++;
        return;
    }

    for (;;) {
        if (fat_read(src, spath, offset, copy_buf, COPY_CHUNK, &done) != FAT_OK) {
            copy_failures++;
            return;
        }

        if (done == 0)
            return;

        if (fat_write(&new_fs, dpath, copy_buf, done, offset != 0) != FAT_OK) {
            copy_failures++;
            return;
        }

        offset += done;
    }
}

static void copy_dir(fat_fs_t *src, const char *spath, const char *dpath,
                     unsigned int depth)
{
    items_t list;
    char s[FAT_PATH_MAX];
    char d[FAT_PATH_MAX];

    list.count = 0;

    if (fat_list(src, spath, collect, &list) != FAT_OK) {
        copy_failures++;
        return;
    }

    for (unsigned int i = 0; i < list.count; i++) {
        if (!join(s, sizeof(s), spath, list.items[i].name) ||
            !join(d, sizeof(d), dpath, list.items[i].name)) {
            copy_failures++;
            continue;
        }

        if (list.items[i].attr & FAT_ATTR_DIRECTORY) {
            if (depth >= COPY_MAX_DEPTH) {
                copy_failures++;
                continue;
            }

            fat_err_t r = fat_mkdir(&new_fs, d);

            if (r != FAT_OK && r != FAT_ERR_EXISTS) {
                copy_failures++;
                continue;
            }

            copy_dir(src, s, d, depth + 1);
        } else {
            copy_file(src, s, d);
        }
    }
}

/* ================================================================
 * Install
 * ================================================================ */

const char *persist_install(unsigned int index, uint32_t size_mib)
{
    blockdev_t *disk = fs_disk_get(index);
    uint64_t free_sectors;

    if (!disk)
        return "no such disk";

    gpt_state_t state = gpt_inspect(disk, &free_sectors);

    if (state == GPT_STATE_OTHER)
        return "disk is not blank and has no valid GPT; PIEG will not touch it";

    uint64_t want = (uint64_t)size_mib * 2048u;

    if (want == 0) {
        want = free_sectors < DEFAULT_MAX_SECTORS ? free_sectors
                                                  : DEFAULT_MAX_SECTORS;
    }

    if (want < MKFAT_MIN_SECTORS)
        return "not enough free space (64 MiB needed)";

    if (want > free_sectors)
        return "not enough free space on that disk";

    const char *err;

    if (state == GPT_STATE_BLANK) {
        err = gpt_create(disk);

        if (err)
            return err;
    }

    uint64_t first, count;

    err = gpt_add_partition(disk, want, gpt_type_pieg, "PIEG", &first, &count);

    if (err)
        return err;

    partition_make(&new_part, disk, first, count, "new");

    err = fat_format32(&new_part.dev, PIEG_VOLUME_LABEL, (uint32_t)first);

    if (err)
        return err;

    if (!fat_mount(&new_fs, &new_part.dev))
        return "new volume failed to mount";

    if (fat_mkdir(&new_fs, PIEG_DIR_SYSTEM) != FAT_OK ||
        fat_mkdir(&new_fs, PIEG_DIR_PROGRAMS) != FAT_OK ||
        fat_mkdir(&new_fs, PIEG_DIR_USERS) != FAT_OK ||
        fat_mkdir(&new_fs, PIEG_DIR_HOME) != FAT_OK)
        return "could not create the PIEG directories";

    /* Bring the running system along so the new volume boots on its own. */
    fat_fs_t *src = fs_root();

    copy_failures = 0;

    if (src && fs_is_pieg_volume())
        copy_dir(src, "/", "/", 0);

    if (copy_failures)
        return "volume created, but some files could not be copied";

    return NULL;
}
