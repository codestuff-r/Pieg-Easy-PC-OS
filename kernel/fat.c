#include "fat.h"
#include "mem.h"
#include <stddef.h>

/*
 * FAT12/16/32 driver: mount, lookup (8.3 and long names), list, read,
 * write/append/truncate, create, mkdir, remove. New entries get 8.3 names
 * only; long names are read and removed correctly.
 */

#define DIR_ENTRY_SIZE   32u
#define FAT_DEFAULT_DATE 0x5C21u    /* 2026-01-01 (no RTC driver yet) */

#define CLUSTER_VALID(fs, c) \
    ((c) >= 2u && (c) < (fs)->cluster_count + 2u)

/* ================================================================
 * Helpers
 * ================================================================ */

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

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t str_len(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

static char up(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (up(*a++) != up(*b++))
            return false;
    }

    return *a == *b;
}

static bool rs(fat_fs_t *fs, uint32_t lba, void *buffer)
{
    return blockdev_read(fs->device, lba, 1, buffer);
}

static bool ws(fat_fs_t *fs, uint32_t lba, const void *buffer)
{
    return fs->writable && blockdev_write(fs->device, lba, 1, buffer);
}

static uint32_t cluster_lba(const fat_fs_t *fs, uint32_t cluster)
{
    return fs->first_data_sector
         + (cluster - 2u) * fs->sectors_per_cluster;
}

const char *fat_error_string(fat_err_t error)
{
    switch (error) {
    case FAT_OK:           return "ok";
    case FAT_ERR_IO:       return "disk I/O error";
    case FAT_ERR_NOFS:     return "no filesystem mounted";
    case FAT_ERR_NOTFOUND: return "no such file or directory";
    case FAT_ERR_EXISTS:   return "already exists";
    case FAT_ERR_NOTDIR:   return "not a directory";
    case FAT_ERR_ISDIR:    return "is a directory";
    case FAT_ERR_NOSPACE:  return "no space left on volume";
    case FAT_ERR_NAME:     return "invalid name (8.3 names only)";
    case FAT_ERR_NOTEMPTY: return "directory not empty";
    case FAT_ERR_READONLY: return "volume is read-only";
    case FAT_ERR_INVALID:  return "invalid argument";
    }

    return "unknown error";
}

/* ================================================================
 * Mount
 * ================================================================ */

bool fat_mount(fat_fs_t *fs, blockdev_t *device)
{
    uint8_t b[512];

    if (!fs || !device || device->sector_size != 512)
        return false;

    if (!blockdev_read(device, 0, 1, b))
        return false;

    if (b[510] != 0x55 || b[511] != 0xAA)
        return false;

    /* Boot sectors start with a jump; this rejects partition tables. */
    if (b[0] != 0xEB && b[0] != 0xE9)
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

    uint32_t root_cluster = 0;

    if (type == FAT_TYPE_32) {
        root_cluster = rd32(&b[44]) & 0x0FFFFFFF;
        if (root_cluster < 2 || root_cluster >= clusters + 2)
            return false;
    }

    memset(fs, 0, sizeof(*fs));

    fs->device = device;
    fs->type = type;
    fs->writable = device->write != 0;
    fs->bytes_per_sector = bps;
    fs->sectors_per_cluster = spc;
    fs->reserved_sectors = reserved;
    fs->fat_count = fats;
    fs->sectors_per_fat = spf;
    fs->root_cluster = root_cluster;
    fs->root_dir_sectors = root_sectors;
    fs->first_fat_sector = reserved;
    fs->first_data_sector = first_data;
    fs->cluster_count = clusters;
    fs->root_dir_sector = reserved + (uint32_t)fats * spf;
    fs->next_free = 2;

    const uint8_t *label = type == FAT_TYPE_32 ? &b[71] : &b[43];
    unsigned int n = 11;

    while (n && (label[n - 1] == ' ' || label[n - 1] == 0))
        n--;

    for (unsigned int i = 0; i < n; i++)
        fs->label[i] = (char)label[i];

    fs->label[n] = '\0';

    return true;
}

/* ================================================================
 * FAT table
 * ================================================================ */

static uint8_t *fat_sector(fat_fs_t *fs, uint32_t sector)
{
    if (!fs->fc_valid || fs->fc_lba != sector) {
        if (!rs(fs, sector, fs->fc_buf)) {
            fs->fc_valid = false;
            return NULL;
        }

        fs->fc_lba = sector;
        fs->fc_valid = true;
    }

    return fs->fc_buf;
}

/* Writes the cached FAT sector to every FAT copy. */
static bool fat_flush(fat_fs_t *fs, uint32_t sector)
{
    for (unsigned int f = 0; f < fs->fat_count; f++) {
        if (!ws(fs, sector + f * fs->sectors_per_fat, fs->fc_buf))
            return false;
    }

    return true;
}

static bool fat_get(fat_fs_t *fs, uint32_t cluster, uint32_t *value)
{
    uint8_t *p;

    if (fs->type == FAT_TYPE_12) {
        uint32_t off = cluster + cluster / 2;
        uint32_t sector = fs->first_fat_sector + off / 512;
        uint32_t i = off % 512;
        uint8_t lo, hi;

        p = fat_sector(fs, sector);
        if (!p)
            return false;

        lo = p[i];

        if (i == 511) {
            p = fat_sector(fs, sector + 1);
            if (!p)
                return false;
            hi = p[0];
        } else {
            hi = p[i + 1];
        }

        uint16_t w = (uint16_t)(lo | (hi << 8));
        *value = (cluster & 1) ? (uint32_t)(w >> 4) : (uint32_t)(w & 0x0FFF);
        return true;
    }

    if (fs->type == FAT_TYPE_16) {
        uint32_t off = cluster * 2;

        p = fat_sector(fs, fs->first_fat_sector + off / 512);
        if (!p)
            return false;

        *value = rd16(&p[off % 512]);
        return true;
    }

    uint32_t off = cluster * 4;

    p = fat_sector(fs, fs->first_fat_sector + off / 512);
    if (!p)
        return false;

    *value = rd32(&p[off % 512]) & 0x0FFFFFFF;
    return true;
}

static bool fat_put_byte(fat_fs_t *fs, uint32_t sector, uint32_t i,
                         uint8_t value)
{
    uint8_t *p = fat_sector(fs, sector);

    if (!p)
        return false;

    p[i] = value;
    return fat_flush(fs, sector);
}

static bool fat_set(fat_fs_t *fs, uint32_t cluster, uint32_t value)
{
    uint8_t *p;

    if (!fs->writable)
        return false;

    if (fs->type == FAT_TYPE_12) {
        uint32_t off = cluster + cluster / 2;
        uint32_t sector = fs->first_fat_sector + off / 512;
        uint32_t i = off % 512;
        uint32_t cur;

        if (!fat_get(fs, cluster, &cur))
            return false;

        /* Rebuild the 16-bit window containing the 12-bit entry. */
        uint8_t lo, hi;

        p = fat_sector(fs, sector);
        if (!p)
            return false;
        lo = p[i];

        if (i == 511) {
            p = fat_sector(fs, sector + 1);
            if (!p)
                return false;
            hi = p[0];
        } else {
            hi = p[i + 1];
        }

        uint16_t w = (uint16_t)(lo | (hi << 8));

        if (cluster & 1)
            w = (uint16_t)((w & 0x000F) | ((value & 0x0FFF) << 4));
        else
            w = (uint16_t)((w & 0xF000) | (value & 0x0FFF));

        if (!fat_put_byte(fs, sector, i, (uint8_t)w))
            return false;

        if (i == 511)
            return fat_put_byte(fs, sector + 1, 0, (uint8_t)(w >> 8));

        return fat_put_byte(fs, sector, i + 1, (uint8_t)(w >> 8));
    }

    if (fs->type == FAT_TYPE_16) {
        uint32_t off = cluster * 2;
        uint32_t sector = fs->first_fat_sector + off / 512;

        p = fat_sector(fs, sector);
        if (!p)
            return false;

        wr16(&p[off % 512], (uint16_t)value);
        return fat_flush(fs, sector);
    }

    uint32_t off = cluster * 4;
    uint32_t sector = fs->first_fat_sector + off / 512;

    p = fat_sector(fs, sector);
    if (!p)
        return false;

    uint32_t old = rd32(&p[off % 512]);
    wr32(&p[off % 512], (old & 0xF0000000u) | (value & 0x0FFFFFFFu));
    return fat_flush(fs, sector);
}

static uint32_t eoc_value(const fat_fs_t *fs)
{
    if (fs->type == FAT_TYPE_12)
        return 0x0FFF;
    if (fs->type == FAT_TYPE_16)
        return 0xFFFF;
    return 0x0FFFFFFF;
}

/* Next cluster in the chain; *next = 0 at the end (or on a bad link). */
static bool fat_next(fat_fs_t *fs, uint32_t cluster, uint32_t *next)
{
    uint32_t v;

    if (!fat_get(fs, cluster, &v))
        return false;

    *next = CLUSTER_VALID(fs, v) ? v : 0;
    return true;
}

static bool chain_last(fat_fs_t *fs, uint32_t start, uint32_t *last)
{
    uint32_t c = start;

    for (uint32_t n = 0; n < fs->cluster_count; n++) {
        uint32_t next;

        if (!fat_next(fs, c, &next))
            return false;

        if (!next) {
            *last = c;
            return true;
        }

        c = next;
    }

    return false;
}

static fat_err_t chain_free(fat_fs_t *fs, uint32_t start)
{
    uint32_t c = start;

    for (uint32_t n = 0; c && n <= fs->cluster_count; n++) {
        uint32_t next;

        if (!fat_next(fs, c, &next))
            return FAT_ERR_IO;

        if (!fat_set(fs, c, 0))
            return FAT_ERR_IO;

        c = next;
    }

    return FAT_OK;
}

static fat_err_t cluster_alloc(fat_fs_t *fs, uint32_t *out)
{
    if (!fs->writable)
        return FAT_ERR_READONLY;

    uint32_t count = fs->cluster_count;
    uint32_t start = fs->next_free;

    if (start < 2 || start >= count + 2)
        start = 2;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t c = 2 + (start - 2 + i) % count;
        uint32_t v;

        if (!fat_get(fs, c, &v))
            return FAT_ERR_IO;

        if (v == 0) {
            if (!fat_set(fs, c, eoc_value(fs)))
                return FAT_ERR_IO;

            fs->next_free = c + 1;
            *out = c;
            return FAT_OK;
        }
    }

    return FAT_ERR_NOSPACE;
}

static fat_err_t cluster_zero(fat_fs_t *fs, uint32_t cluster)
{
    uint8_t zero[512];

    memset(zero, 0, sizeof(zero));

    for (uint32_t i = 0; i < fs->sectors_per_cluster; i++) {
        if (!ws(fs, cluster_lba(fs, cluster) + i, zero))
            return FAT_ERR_IO;
    }

    return FAT_OK;
}

fat_err_t fat_stats(fat_fs_t *fs, uint32_t *total_clusters,
                    uint32_t *free_clusters)
{
    uint32_t free_count = 0;

    if (!fs || !fs->device)
        return FAT_ERR_NOFS;

    for (uint32_t c = 2; c < fs->cluster_count + 2; c++) {
        uint32_t v;

        if (!fat_get(fs, c, &v))
            return FAT_ERR_IO;

        if (v == 0)
            free_count++;
    }

    if (total_clusters)
        *total_clusters = fs->cluster_count;
    if (free_clusters)
        *free_clusters = free_count;

    return FAT_OK;
}

/* ================================================================
 * Directories
 * ================================================================ */

/*
 * Maps sector `index` of a directory to an LBA. dir == 0 is the root.
 * Returns FAT_ERR_NOTFOUND once the directory has no more sectors.
 */
static fat_err_t dir_sector(fat_fs_t *fs, uint32_t dir, uint32_t index,
                            uint32_t *lba)
{
    if (dir == 0 && fs->type != FAT_TYPE_32) {
        if (index >= fs->root_dir_sectors)
            return FAT_ERR_NOTFOUND;

        *lba = fs->root_dir_sector + index;
        return FAT_OK;
    }

    uint32_t c = dir ? dir : fs->root_cluster;
    uint32_t hops = index / fs->sectors_per_cluster;

    while (hops--) {
        uint32_t next;

        if (!fat_next(fs, c, &next))
            return FAT_ERR_IO;

        if (!next)
            return FAT_ERR_NOTFOUND;

        c = next;
    }

    *lba = cluster_lba(fs, c) + index % fs->sectors_per_cluster;
    return FAT_OK;
}

static void format_short(const uint8_t *d, char out[13])
{
    unsigned int p = 0;
    unsigned int end = 8;
    bool lower_base = (d[12] & 0x08) != 0;
    bool lower_ext = (d[12] & 0x10) != 0;

    while (end && d[end - 1] == ' ')
        end--;

    for (unsigned int i = 0; i < end; i++) {
        char c = (char)d[i];

        if (i == 0 && d[0] == 0x05)
            c = (char)0xE5;
        if (lower_base && c >= 'A' && c <= 'Z')
            c = (char)(c + 32);

        out[p++] = c;
    }

    end = 3;
    while (end && d[8 + end - 1] == ' ')
        end--;

    if (end) {
        out[p++] = '.';
        for (unsigned int i = 0; i < end; i++) {
            char c = (char)d[8 + i];

            if (lower_ext && c >= 'A' && c <= 'Z')
                c = (char)(c + 32);

            out[p++] = c;
        }
    }

    out[p] = '\0';
}

static uint8_t lfn_checksum(const uint8_t *d)
{
    uint8_t sum = 0;

    for (unsigned int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) ? 0x80 : 0) + (sum >> 1) + d[i]);

    return sum;
}

typedef bool (*walk_fn)(const fat_dirent_t *entry, void *context);

/* Visits every live entry. The callback returns true to stop early. */
static fat_err_t dir_walk(fat_fs_t *fs, uint32_t dir, walk_fn fn,
                          void *context)
{
    static const uint8_t lfn_pos[13] =
        { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };

    uint8_t b[512];
    fat_dirent_t e;
    char lfn[272];
    bool lfn_on = false;
    uint8_t lfn_sum = 0;
    uint8_t lfn_next = 0;

    e.lfn_count = 0;

    for (uint32_t index = 0;; index++) {
        uint32_t lba;
        fat_err_t r = dir_sector(fs, dir, index, &lba);

        if (r == FAT_ERR_NOTFOUND)
            return FAT_OK;
        if (r != FAT_OK)
            return r;

        if (!rs(fs, lba, b))
            return FAT_ERR_IO;

        for (unsigned int off = 0; off < 512; off += DIR_ENTRY_SIZE) {
            const uint8_t *d = &b[off];

            if (d[0] == 0x00)
                return FAT_OK;

            if (d[0] == 0xE5) {
                lfn_on = false;
                continue;
            }

            if (d[11] == FAT_ATTR_LONG_NAME) {
                uint8_t ord = d[0] & 0x3F;

                if (d[0] & 0x40) {
                    lfn_on = ord >= 1 && ord <= 20;
                    lfn_sum = d[13];
                    lfn_next = ord;
                    e.lfn_count = 0;
                    memset(lfn, 0, sizeof(lfn));
                } else if (!lfn_on || ord < 1 ||
                           ord != (uint8_t)(lfn_next - 1) ||
                           d[13] != lfn_sum) {
                    lfn_on = false;
                }

                if (!lfn_on)
                    continue;

                lfn_next = ord;

                if (e.lfn_count < 20) {
                    e.lfn_lba[e.lfn_count] = lba;
                    e.lfn_off[e.lfn_count] = (uint16_t)off;
                    e.lfn_count++;
                }

                for (unsigned int k = 0; k < 13; k++) {
                    uint16_t ch = rd16(&d[lfn_pos[k]]);
                    char *slot = &lfn[(ord - 1u) * 13u + k];

                    if (ch == 0 || ch == 0xFFFF)
                        *slot = '\0';
                    else if (ch < 0x80)
                        *slot = (char)ch;
                    else
                        *slot = '?';
                }

                continue;
            }

            if (d[11] & FAT_ATTR_VOLUME_ID) {
                lfn_on = false;
                continue;
            }

            format_short(d, e.short_name);

            if (lfn_on && lfn_next == 1 && lfn_sum == lfn_checksum(d)) {
                unsigned int n = 0;

                while (lfn[n] && n < FAT_NAME_MAX) {
                    e.name[n] = lfn[n];
                    n++;
                }

                e.name[n] = '\0';
            } else {
                e.lfn_count = 0;

                unsigned int n = 0;

                while (e.short_name[n]) {
                    e.name[n] = e.short_name[n];
                    n++;
                }

                e.name[n] = '\0';
            }

            lfn_on = false;

            e.attr = d[11];
            e.size = rd32(&d[28]);
            e.cluster = rd16(&d[26]);

            if (fs->type == FAT_TYPE_32)
                e.cluster |= (uint32_t)rd16(&d[20]) << 16;

            e.dir_cluster = dir;
            e.entry_lba = lba;
            e.entry_off = (uint16_t)off;

            if (fn(&e, context))
                return FAT_OK;
        }
    }
}

struct find_ctx {
    const char *name;
    fat_dirent_t *out;
    bool found;
};

static bool find_cb(const fat_dirent_t *e, void *context)
{
    struct find_ctx *f = (struct find_ctx *)context;

    if (name_eq(e->name, f->name) || name_eq(e->short_name, f->name)) {
        *f->out = *e;
        f->found = true;
        return true;
    }

    return false;
}

static void root_entry(const fat_fs_t *fs, fat_dirent_t *e)
{
    memset(e, 0, sizeof(*e));
    e->name[0] = '/';
    e->attr = FAT_ATTR_DIRECTORY;
    e->cluster = fs->type == FAT_TYPE_32 ? fs->root_cluster : 0;
}

static bool is_root(const fat_dirent_t *e)
{
    return e->entry_lba == 0;
}

fat_err_t fat_lookup(fat_fs_t *fs, const char *path, fat_dirent_t *out)
{
    fat_dirent_t cur;
    fat_dirent_t next;
    char comp[FAT_NAME_MAX + 1];

    if (!fs || !fs->device || !path || !out)
        return FAT_ERR_INVALID;

    root_entry(fs, &cur);

    const char *p = path;

    for (;;) {
        while (*p == '/')
            p++;

        if (!*p)
            break;

        size_t n = 0;

        while (*p && *p != '/') {
            if (n >= FAT_NAME_MAX)
                return FAT_ERR_NAME;
            comp[n++] = *p++;
        }

        comp[n] = '\0';

        if (!(cur.attr & FAT_ATTR_DIRECTORY))
            return FAT_ERR_NOTDIR;

        struct find_ctx f = { comp, &next, false };
        fat_err_t r = dir_walk(fs, cur.cluster, find_cb, &f);

        if (r != FAT_OK)
            return r;

        if (!f.found)
            return FAT_ERR_NOTFOUND;

        cur = next;
    }

    *out = cur;
    return FAT_OK;
}

struct list_ctx {
    fat_dir_callback_t callback;
    void *context;
};

static bool list_cb(const fat_dirent_t *e, void *context)
{
    struct list_ctx *l = (struct list_ctx *)context;

    if (e->name[0] == '.' &&
        (e->name[1] == '\0' || (e->name[1] == '.' && e->name[2] == '\0')))
        return false;

    l->callback(e->name, e->attr, e->size, l->context);
    return false;
}

fat_err_t fat_list(fat_fs_t *fs, const char *path,
                   fat_dir_callback_t callback, void *context)
{
    fat_dirent_t dir;

    if (!fs || !fs->device || !callback)
        return FAT_ERR_INVALID;

    fat_err_t r = fat_lookup(fs, path, &dir);

    if (r != FAT_OK)
        return r;

    if (!(dir.attr & FAT_ATTR_DIRECTORY))
        return FAT_ERR_NOTDIR;

    struct list_ctx l = { callback, context };

    return dir_walk(fs, dir.cluster, list_cb, &l);
}

/* ================================================================
 * Reading
 * ================================================================ */

fat_err_t fat_read(fat_fs_t *fs, const char *path, uint32_t offset,
                   void *buffer, uint32_t length, uint32_t *done)
{
    fat_dirent_t e;
    uint8_t sec[512];

    if (done)
        *done = 0;

    if (!fs || !fs->device || !buffer)
        return FAT_ERR_INVALID;

    fat_err_t r = fat_lookup(fs, path, &e);

    if (r != FAT_OK)
        return r;

    if (e.attr & FAT_ATTR_DIRECTORY)
        return FAT_ERR_ISDIR;

    if (offset >= e.size || length == 0)
        return FAT_OK;

    if (length > e.size - offset)
        length = e.size - offset;

    uint32_t cb = fs->sectors_per_cluster * 512u;
    uint32_t cl = e.cluster;
    uint32_t skip = offset / cb;

    while (skip-- && cl) {
        uint32_t next;

        if (!fat_next(fs, cl, &next))
            return FAT_ERR_IO;

        cl = next;
    }

    if (!cl)
        return FAT_ERR_IO;

    uint32_t in = offset % cb;
    uint32_t total = 0;
    uint8_t *dst = (uint8_t *)buffer;

    while (length) {
        while (in < cb && length) {
            uint32_t so = in % 512;
            uint32_t chunk = 512 - so;
            uint32_t lba = cluster_lba(fs, cl) + in / 512;

            if (chunk > length)
                chunk = length;

            if (chunk == 512) {
                if (!rs(fs, lba, dst))
                    return FAT_ERR_IO;
            } else {
                if (!rs(fs, lba, sec))
                    return FAT_ERR_IO;
                memcpy(dst, sec + so, chunk);
            }

            dst += chunk;
            in += chunk;
            length -= chunk;
            total += chunk;
        }

        if (length) {
            uint32_t next;

            if (!fat_next(fs, cl, &next))
                return FAT_ERR_IO;

            if (!next) {
                if (done)
                    *done = total;
                return FAT_ERR_IO;
            }

            cl = next;
            in = 0;
        }
    }

    if (done)
        *done = total;

    return FAT_OK;
}

/* ================================================================
 * Creating, writing, removing
 * ================================================================ */

static bool short_char_ok(char c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
        (c >= '0' && c <= '9'))
        return true;

    for (const char *p = "$%'-_@~`!(){}^#&"; *p; p++) {
        if (*p == c)
            return true;
    }

    return false;
}

static bool make_short(const char *name, uint8_t out[11], uint8_t *flags)
{
    size_t len = str_len(name);
    int dot = -1;

    if (len == 0 || len > 12)
        return false;

    for (size_t i = 0; i < len; i++) {
        if (name[i] == '.')
            dot = (int)i;
    }

    size_t base_len = dot >= 0 ? (size_t)dot : len;
    size_t ext_len = dot >= 0 ? len - (size_t)dot - 1 : 0;

    if (base_len == 0 || base_len > 8 || ext_len > 3)
        return false;

    if (dot >= 0 && ext_len == 0)
        return false;

    memset(out, ' ', 11);

    bool base_lower = false, base_upper = false;
    bool ext_lower = false, ext_upper = false;

    for (size_t i = 0; i < base_len; i++) {
        char c = name[i];

        if (!short_char_ok(c))
            return false;

        if (c >= 'a' && c <= 'z')
            base_lower = true;
        if (c >= 'A' && c <= 'Z')
            base_upper = true;

        out[i] = (uint8_t)up(c);
    }

    for (size_t i = 0; i < ext_len; i++) {
        char c = name[base_len + 1 + i];

        if (!short_char_ok(c))
            return false;

        if (c >= 'a' && c <= 'z')
            ext_lower = true;
        if (c >= 'A' && c <= 'Z')
            ext_upper = true;

        out[8 + i] = (uint8_t)up(c);
    }

    *flags = 0;

    if (base_lower && !base_upper)
        *flags |= 0x08;
    if (ext_lower && !ext_upper)
        *flags |= 0x10;

    return true;
}

static fat_err_t dir_find_free(fat_fs_t *fs, uint32_t dir,
                               uint32_t *lba_out, uint16_t *off_out)
{
    uint8_t b[512];

    for (uint32_t index = 0;; index++) {
        uint32_t lba;
        fat_err_t r = dir_sector(fs, dir, index, &lba);

        if (r == FAT_ERR_NOTFOUND)
            break;
        if (r != FAT_OK)
            return r;

        if (!rs(fs, lba, b))
            return FAT_ERR_IO;

        for (unsigned int off = 0; off < 512; off += DIR_ENTRY_SIZE) {
            if (b[off] == 0x00 || b[off] == 0xE5) {
                *lba_out = lba;
                *off_out = (uint16_t)off;
                return FAT_OK;
            }
        }
    }

    /* The fixed FAT12/16 root cannot grow. */
    if (dir == 0 && fs->type != FAT_TYPE_32)
        return FAT_ERR_NOSPACE;

    uint32_t last;

    if (!chain_last(fs, dir ? dir : fs->root_cluster, &last))
        return FAT_ERR_IO;

    uint32_t c;
    fat_err_t r = cluster_alloc(fs, &c);

    if (r != FAT_OK)
        return r;

    r = cluster_zero(fs, c);

    if (r != FAT_OK || !fat_set(fs, last, c)) {
        fat_set(fs, c, 0);
        return r != FAT_OK ? r : FAT_ERR_IO;
    }

    *lba_out = cluster_lba(fs, c);
    *off_out = 0;
    return FAT_OK;
}

static fat_err_t dir_add(fat_fs_t *fs, uint32_t dir, const char *name,
                         uint8_t attr, uint32_t cluster,
                         fat_dirent_t *out)
{
    uint8_t sn[11];
    uint8_t flags;
    uint8_t b[512];
    fat_dirent_t tmp;

    if (!make_short(name, sn, &flags))
        return FAT_ERR_NAME;

    if (!fs->writable)
        return FAT_ERR_READONLY;

    struct find_ctx f = { name, &tmp, false };
    fat_err_t r = dir_walk(fs, dir, find_cb, &f);

    if (r != FAT_OK)
        return r;

    if (f.found)
        return FAT_ERR_EXISTS;

    uint32_t lba;
    uint16_t off;

    r = dir_find_free(fs, dir, &lba, &off);

    if (r != FAT_OK)
        return r;

    if (!rs(fs, lba, b))
        return FAT_ERR_IO;

    uint8_t *d = &b[off];

    memset(d, 0, DIR_ENTRY_SIZE);
    memcpy(d, sn, 11);
    d[11] = attr;
    d[12] = flags;
    wr16(&d[16], FAT_DEFAULT_DATE);
    wr16(&d[18], FAT_DEFAULT_DATE);
    wr16(&d[20], fs->type == FAT_TYPE_32 ? (uint16_t)(cluster >> 16) : 0);
    wr16(&d[24], FAT_DEFAULT_DATE);
    wr16(&d[26], (uint16_t)cluster);
    wr32(&d[28], 0);

    if (!ws(fs, lba, b))
        return FAT_ERR_IO;

    if (out) {
        memset(out, 0, sizeof(*out));

        unsigned int n = 0;
        format_short(d, out->short_name);

        while (out->short_name[n]) {
            out->name[n] = out->short_name[n];
            n++;
        }

        out->attr = attr;
        out->cluster = cluster;
        out->dir_cluster = dir;
        out->entry_lba = lba;
        out->entry_off = off;
    }

    return FAT_OK;
}

static fat_err_t update_entry(fat_fs_t *fs, const fat_dirent_t *e)
{
    uint8_t b[512];

    if (!rs(fs, e->entry_lba, b))
        return FAT_ERR_IO;

    uint8_t *d = &b[e->entry_off];

    d[11] = e->attr;

    if (fs->type == FAT_TYPE_32)
        wr16(&d[20], (uint16_t)(e->cluster >> 16));

    wr16(&d[24], FAT_DEFAULT_DATE);
    wr16(&d[26], (uint16_t)e->cluster);
    wr32(&d[28], e->size);

    return ws(fs, e->entry_lba, b) ? FAT_OK : FAT_ERR_IO;
}

/* Splits "/A/B/C" into parent "/A/B" and name "C". */
static fat_err_t split_path(const char *path, char *parent, char *name)
{
    size_t n = str_len(path);

    while (n > 1 && path[n - 1] == '/')
        n--;

    size_t i = n;

    while (i > 0 && path[i - 1] != '/')
        i--;

    if (i == n)
        return FAT_ERR_INVALID;

    if (n - i > FAT_NAME_MAX || i > FAT_PATH_MAX)
        return FAT_ERR_NAME;

    for (size_t k = 0; k < n - i; k++)
        name[k] = path[i + k];
    name[n - i] = '\0';

    size_t plen = i > 1 ? i - 1 : i;

    if (plen == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        for (size_t k = 0; k < plen; k++)
            parent[k] = path[k];
        parent[plen] = '\0';
    }

    return FAT_OK;
}

static fat_err_t parent_dir(fat_fs_t *fs, const char *path,
                            fat_dirent_t *parent, char *name)
{
    char parent_path[FAT_PATH_MAX + 1];
    fat_err_t r = split_path(path, parent_path, name);

    if (r != FAT_OK)
        return r;

    r = fat_lookup(fs, parent_path, parent);

    if (r != FAT_OK)
        return r;

    if (!(parent->attr & FAT_ATTR_DIRECTORY))
        return FAT_ERR_NOTDIR;

    return FAT_OK;
}

static fat_err_t create_file(fat_fs_t *fs, const char *path,
                             fat_dirent_t *out)
{
    fat_dirent_t parent;
    char name[FAT_NAME_MAX + 1];

    fat_err_t r = parent_dir(fs, path, &parent, name);

    if (r != FAT_OK)
        return r;

    return dir_add(fs, parent.cluster, name, FAT_ATTR_ARCHIVE, 0, out);
}

fat_err_t fat_create(fat_fs_t *fs, const char *path)
{
    if (!fs || !fs->device || !path)
        return FAT_ERR_INVALID;

    if (!fs->writable)
        return FAT_ERR_READONLY;

    return create_file(fs, path, NULL);
}

fat_err_t fat_mkdir(fat_fs_t *fs, const char *path)
{
    fat_dirent_t parent;
    fat_dirent_t existing;
    char name[FAT_NAME_MAX + 1];
    uint8_t b[512];

    if (!fs || !fs->device || !path)
        return FAT_ERR_INVALID;

    if (!fs->writable)
        return FAT_ERR_READONLY;

    fat_err_t r = parent_dir(fs, path, &parent, name);

    if (r != FAT_OK)
        return r;

    if (fat_lookup(fs, path, &existing) == FAT_OK)
        return FAT_ERR_EXISTS;

    uint8_t sn[11];
    uint8_t flags;

    if (!make_short(name, sn, &flags))
        return FAT_ERR_NAME;

    uint32_t c;

    r = cluster_alloc(fs, &c);

    if (r != FAT_OK)
        return r;

    r = cluster_zero(fs, c);

    if (r == FAT_OK) {
        uint32_t parent_cluster = is_root(&parent) ? 0 : parent.cluster;

        memset(b, 0, sizeof(b));

        memset(&b[0], ' ', 11);
        b[0] = '.';
        b[11] = FAT_ATTR_DIRECTORY;
        wr16(&b[16], FAT_DEFAULT_DATE);
        wr16(&b[18], FAT_DEFAULT_DATE);
        wr16(&b[20], fs->type == FAT_TYPE_32 ? (uint16_t)(c >> 16) : 0);
        wr16(&b[24], FAT_DEFAULT_DATE);
        wr16(&b[26], (uint16_t)c);

        memset(&b[32], ' ', 11);
        b[32] = '.';
        b[33] = '.';
        b[43] = FAT_ATTR_DIRECTORY;
        wr16(&b[48], FAT_DEFAULT_DATE);
        wr16(&b[50], FAT_DEFAULT_DATE);
        wr16(&b[52], fs->type == FAT_TYPE_32
                     ? (uint16_t)(parent_cluster >> 16) : 0);
        wr16(&b[56], FAT_DEFAULT_DATE);
        wr16(&b[58], (uint16_t)parent_cluster);

        if (!ws(fs, cluster_lba(fs, c), b))
            r = FAT_ERR_IO;
    }

    if (r == FAT_OK)
        r = dir_add(fs, parent.cluster, name, FAT_ATTR_DIRECTORY, c, NULL);

    if (r != FAT_OK)
        chain_free(fs, c);

    return r;
}

static fat_err_t cluster_write(fat_fs_t *fs, uint32_t cluster,
                               uint32_t offset, const uint8_t *src,
                               uint32_t count)
{
    uint8_t b[512];
    uint32_t lba = cluster_lba(fs, cluster) + offset / 512;
    uint32_t so = offset % 512;

    while (count) {
        uint32_t chunk = 512 - so;

        if (chunk > count)
            chunk = count;

        if (chunk == 512) {
            if (!ws(fs, lba, src))
                return FAT_ERR_IO;
        } else {
            if (!rs(fs, lba, b))
                return FAT_ERR_IO;

            memcpy(b + so, src, chunk);

            if (!ws(fs, lba, b))
                return FAT_ERR_IO;
        }

        lba++;
        src += chunk;
        count -= chunk;
        so = 0;
    }

    return FAT_OK;
}

fat_err_t fat_write(fat_fs_t *fs, const char *path, const void *data,
                    uint32_t length, bool append)
{
    fat_dirent_t e;

    if (!fs || !fs->device || !path || (!data && length))
        return FAT_ERR_INVALID;

    if (!fs->writable)
        return FAT_ERR_READONLY;

    fat_err_t r = fat_lookup(fs, path, &e);

    if (r == FAT_ERR_NOTFOUND)
        r = create_file(fs, path, &e);

    if (r != FAT_OK)
        return r;

    if (e.attr & FAT_ATTR_DIRECTORY)
        return FAT_ERR_ISDIR;

    if (!append) {
        if (e.cluster) {
            r = chain_free(fs, e.cluster);

            if (r != FAT_OK)
                return r;
        }

        e.cluster = 0;
        e.size = 0;
    }

    if (length > 0xFFFFFFFFu - e.size)
        return FAT_ERR_NOSPACE;

    uint32_t cb = fs->sectors_per_cluster * 512u;
    uint32_t last = 0;

    if (e.cluster && !chain_last(fs, e.cluster, &last))
        return FAT_ERR_IO;

    const uint8_t *src = (const uint8_t *)data;
    uint32_t pos = e.size;
    uint32_t written = 0;

    r = FAT_OK;

    while (written < length) {
        uint32_t in = pos % cb;

        if (!last || (in == 0 && pos > 0)) {
            uint32_t c;

            r = cluster_alloc(fs, &c);

            if (r != FAT_OK)
                break;

            if (last) {
                if (!fat_set(fs, last, c)) {
                    fat_set(fs, c, 0);
                    r = FAT_ERR_IO;
                    break;
                }
            } else {
                e.cluster = c;
            }

            last = c;
            in = 0;
        }

        uint32_t chunk = cb - in;

        if (chunk > length - written)
            chunk = length - written;

        r = cluster_write(fs, last, in, src + written, chunk);

        if (r != FAT_OK)
            break;

        pos += chunk;
        written += chunk;
    }

    e.size = pos;

    fat_err_t u = update_entry(fs, &e);

    return r != FAT_OK ? r : u;
}

struct empty_ctx {
    bool has_entries;
};

static bool empty_cb(const fat_dirent_t *e, void *context)
{
    struct empty_ctx *c = (struct empty_ctx *)context;

    if (e->name[0] == '.' &&
        (e->name[1] == '\0' || (e->name[1] == '.' && e->name[2] == '\0')))
        return false;

    c->has_entries = true;
    return true;
}

fat_err_t fat_remove(fat_fs_t *fs, const char *path)
{
    fat_dirent_t e;
    uint8_t b[512];

    if (!fs || !fs->device || !path)
        return FAT_ERR_INVALID;

    if (!fs->writable)
        return FAT_ERR_READONLY;

    fat_err_t r = fat_lookup(fs, path, &e);

    if (r != FAT_OK)
        return r;

    if (is_root(&e))
        return FAT_ERR_INVALID;

    if (e.attr & FAT_ATTR_DIRECTORY) {
        struct empty_ctx c = { false };

        r = dir_walk(fs, e.cluster, empty_cb, &c);

        if (r != FAT_OK)
            return r;

        if (c.has_entries)
            return FAT_ERR_NOTEMPTY;
    }

    /* Mark the 8.3 entry and every long-name entry in front of it. */
    for (unsigned int i = 0; i <= e.lfn_count; i++) {
        uint32_t lba = i < e.lfn_count ? e.lfn_lba[i] : e.entry_lba;
        uint16_t off = i < e.lfn_count ? e.lfn_off[i] : e.entry_off;

        if (!rs(fs, lba, b))
            return FAT_ERR_IO;

        b[off] = 0xE5;

        if (!ws(fs, lba, b))
            return FAT_ERR_IO;
    }

    if (e.cluster)
        return chain_free(fs, e.cluster);

    return FAT_OK;
}
