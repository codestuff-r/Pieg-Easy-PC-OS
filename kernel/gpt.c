#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "gpt.h"
#include "mem.h"

#define GPT_ENTRY_SIZE      128u
#define GPT_ENTRY_COUNT     128u
#define GPT_ENTRY_SECTORS   ((GPT_ENTRY_COUNT * GPT_ENTRY_SIZE) / 512u)
#define GPT_HEADER_SIZE     92u
#define ALIGN_SECTORS       2048u           /* 1 MiB */

/* acd36828-1ab3-481f-8d9c-04a8afdc3f41, mixed-endian as GPT stores it. */
const uint8_t gpt_type_pieg[16] = {
    0x28, 0x68, 0xD3, 0xAC, 0xB3, 0x1A, 0x1F, 0x48,
    0x8D, 0x9C, 0x04, 0xA8, 0xAF, 0xDC, 0x3F, 0x41
};

typedef struct {
    uint8_t hdr[512];
    uint64_t my_lba;
    uint64_t alt_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint64_t entries_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint32_t entry_sectors;
} gpt_t;

static uint8_t entries[GPT_ENTRY_COUNT * GPT_ENTRY_SIZE];

/* ================================================================
 * Little-endian helpers and CRC32
 * ================================================================ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void wr64(uint8_t *p, uint64_t v)
{
    wr32(p, (uint32_t)v);
    wr32(p + 4, (uint32_t)(v >> 32));
}

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xFFFFFFFFu;

    while (length--) {
        crc ^= *data++;

        for (unsigned int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int32_t)(crc & 1u));
    }

    return ~crc;
}

static bool is_zero(const uint8_t *p, size_t n)
{
    while (n--) {
        if (*p++)
            return false;
    }

    return true;
}

static uint64_t align_up(uint64_t v)
{
    return (v + ALIGN_SECTORS - 1u) & ~(uint64_t)(ALIGN_SECTORS - 1u);
}

/* Not cryptographic: just needs to differ between disks and runs. */
static void random_guid(uint8_t out[16])
{
    static uint64_t state;
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    state ^= ((uint64_t)hi << 32 | lo) + 0x9E3779B97F4A7C15ull;

    for (unsigned int i = 0; i < 16; i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = (uint8_t)(state >> 24);
    }

    out[7] = (uint8_t)((out[7] & 0x0F) | 0x40);     /* version 4 */
    out[8] = (uint8_t)((out[8] & 0x3F) | 0x80);     /* RFC 4122 variant */
}

/* ================================================================
 * Loading and validating
 * ================================================================ */

static bool load(blockdev_t *disk, gpt_t *g)
{
    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

    if (!blockdev_read(disk, 1, 1, g->hdr))
        return false;

    if (memcmp(g->hdr, sig, 8) != 0)
        return false;

    uint32_t size = rd32(&g->hdr[12]);

    if (size < GPT_HEADER_SIZE || size > 512)
        return false;

    uint32_t stored = rd32(&g->hdr[16]);
    uint8_t copy[512];

    memcpy(copy, g->hdr, 512);
    wr32(&copy[16], 0);

    if (crc32(copy, size) != stored)
        return false;

    g->my_lba = rd64(&g->hdr[24]);
    g->alt_lba = rd64(&g->hdr[32]);
    g->first_usable = rd64(&g->hdr[40]);
    g->last_usable = rd64(&g->hdr[48]);
    g->entries_lba = rd64(&g->hdr[72]);
    g->entry_count = rd32(&g->hdr[80]);
    g->entry_size = rd32(&g->hdr[84]);

    if (g->my_lba != 1 || g->alt_lba != disk->sector_count - 1)
        return false;       /* e.g. image copied to a bigger disk */

    if (g->entry_count != GPT_ENTRY_COUNT || g->entry_size != GPT_ENTRY_SIZE)
        return false;       /* unusual layout: leave it alone */

    g->entry_sectors = GPT_ENTRY_SECTORS;

    if (g->entries_lba < 2 ||
        g->entries_lba + g->entry_sectors > g->first_usable ||
        g->first_usable > g->last_usable ||
        g->last_usable >= g->alt_lba)
        return false;

    if (!blockdev_read(disk, g->entries_lba, g->entry_sectors, entries))
        return false;

    return crc32(entries, sizeof(entries)) == rd32(&g->hdr[88]);
}

typedef struct {
    uint64_t first;
    uint64_t last;
} range_t;

/* Finds the largest aligned gap; returns its start in *start. */
static uint64_t largest_gap(const gpt_t *g, uint64_t *start)
{
    range_t used[GPT_ENTRY_COUNT];
    unsigned int n = 0;

    for (unsigned int i = 0; i < g->entry_count; i++) {
        const uint8_t *e = &entries[i * g->entry_size];

        if (is_zero(e, 16))
            continue;

        range_t r = { rd64(&e[32]), rd64(&e[40]) };

        /* insertion sort by first LBA */
        unsigned int k = n++;

        while (k && used[k - 1].first > r.first) {
            used[k] = used[k - 1];
            k--;
        }

        used[k] = r;
    }

    uint64_t best = 0;
    uint64_t best_start = 0;
    uint64_t cursor = align_up(g->first_usable);

    for (unsigned int i = 0; i < n; i++) {
        if (used[i].first > cursor) {
            uint64_t gap = (used[i].first - cursor) & ~(uint64_t)(ALIGN_SECTORS - 1u);

            if (gap > best) {
                best = gap;
                best_start = cursor;
            }
        }

        uint64_t next = align_up(used[i].last + 1u);

        if (next > cursor)
            cursor = next;
    }

    if (g->last_usable + 1u > cursor) {
        uint64_t gap = (g->last_usable + 1u - cursor) &
                       ~(uint64_t)(ALIGN_SECTORS - 1u);

        if (gap > best) {
            best = gap;
            best_start = cursor;
        }
    }

    *start = best_start;
    return best;
}

static bool region_blank(blockdev_t *disk, uint64_t lba, uint32_t count)
{
    uint8_t b[512];

    for (uint32_t i = 0; i < count; i++) {
        if (!blockdev_read(disk, lba + i, 1, b))
            return false;

        if (!is_zero(b, 512))
            return false;
    }

    return true;
}

gpt_state_t gpt_inspect(blockdev_t *disk, uint64_t *largest_free)
{
    static gpt_t g;
    uint64_t start;

    if (largest_free)
        *largest_free = 0;

    if (!disk || disk->sector_size != 512 || !disk->write ||
        disk->sector_count < 2u * ALIGN_SECTORS)
        return GPT_STATE_OTHER;

    if (load(disk, &g)) {
        if (largest_free)
            *largest_free = largest_gap(&g, &start);

        return GPT_STATE_VALID;
    }

    if (region_blank(disk, 0, 34) &&
        region_blank(disk, disk->sector_count - 33, 33)) {
        if (largest_free) {
            uint64_t last_usable = disk->sector_count - 34;
            uint64_t first = ALIGN_SECTORS;

            *largest_free = (last_usable + 1u - first) &
                            ~(uint64_t)(ALIGN_SECTORS - 1u);
        }

        return GPT_STATE_BLANK;
    }

    return GPT_STATE_OTHER;
}

/* ================================================================
 * Writing
 * ================================================================ */

static void seal_header(uint8_t hdr[512], uint32_t entries_crc)
{
    wr32(&hdr[88], entries_crc);
    wr32(&hdr[16], 0);
    wr32(&hdr[16], crc32(hdr, GPT_HEADER_SIZE));
}

/*
 * Write order keeps at least one valid copy on disk at every step:
 * backup entries, backup header, primary entries, primary header.
 */
static const char *store(blockdev_t *disk, const gpt_t *g)
{
    uint8_t backup[512];
    uint32_t entries_crc = crc32(entries, sizeof(entries));
    uint64_t backup_entries_lba = g->alt_lba - g->entry_sectors;

    uint8_t primary[512];

    memcpy(primary, g->hdr, 512);
    seal_header(primary, entries_crc);

    memcpy(backup, g->hdr, 512);
    wr64(&backup[24], g->alt_lba);
    wr64(&backup[32], g->my_lba);
    wr64(&backup[72], backup_entries_lba);
    seal_header(backup, entries_crc);

    if (!blockdev_write(disk, backup_entries_lba, g->entry_sectors, entries))
        return "disk write failed (backup entries)";

    if (!blockdev_write(disk, g->alt_lba, 1, backup))
        return "disk write failed (backup header)";

    if (!blockdev_write(disk, g->entries_lba, g->entry_sectors, entries))
        return "disk write failed (entries)";

    if (!blockdev_write(disk, g->my_lba, 1, primary))
        return "disk write failed (header)";

    return NULL;
}

const char *gpt_create(blockdev_t *disk)
{
    static gpt_t g;
    uint64_t free_sectors;

    if (gpt_inspect(disk, &free_sectors) != GPT_STATE_BLANK)
        return "disk is not blank";

    uint64_t last = disk->sector_count - 1u;

    memset(&g, 0, sizeof(g));
    memset(entries, 0, sizeof(entries));

    g.my_lba = 1;
    g.alt_lba = last;
    g.entries_lba = 2;
    g.entry_count = GPT_ENTRY_COUNT;
    g.entry_size = GPT_ENTRY_SIZE;
    g.entry_sectors = GPT_ENTRY_SECTORS;
    g.first_usable = 2u + GPT_ENTRY_SECTORS;
    g.last_usable = last - GPT_ENTRY_SECTORS - 1u;

    uint8_t *h = g.hdr;

    memcpy(h, "EFI PART", 8);
    wr32(&h[8], 0x00010000u);
    wr32(&h[12], GPT_HEADER_SIZE);
    wr64(&h[24], g.my_lba);
    wr64(&h[32], g.alt_lba);
    wr64(&h[40], g.first_usable);
    wr64(&h[48], g.last_usable);
    random_guid(&h[56]);
    wr64(&h[72], g.entries_lba);
    wr32(&h[80], g.entry_count);
    wr32(&h[84], g.entry_size);

    const char *err = store(disk, &g);

    if (err)
        return err;

    /* Protective MBR last: until it exists the disk still reads as blank-ish. */
    uint8_t mbr[512];

    memset(mbr, 0, sizeof(mbr));

    uint8_t *e = &mbr[446];
    uint64_t span = last > 0xFFFFFFFFull ? 0xFFFFFFFFull : last;

    e[0] = 0x00;
    e[1] = 0x00; e[2] = 0x02; e[3] = 0x00;      /* CHS start 0/0/2 */
    e[4] = 0xEE;
    e[5] = 0xFF; e[6] = 0xFF; e[7] = 0xFF;      /* CHS end */
    wr32(&e[8], 1);
    wr32(&e[12], (uint32_t)span);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (!blockdev_write(disk, 0, 1, mbr))
        return "disk write failed (protective MBR)";

    return NULL;
}

const char *gpt_add_partition(blockdev_t *disk, uint64_t want,
                              const uint8_t type_guid[16], const char *name,
                              uint64_t *first_lba, uint64_t *sector_count)
{
    static gpt_t g;

    if (gpt_inspect(disk, NULL) != GPT_STATE_VALID)
        return "disk has no valid GPT";

    if (!load(disk, &g))
        return "could not read the GPT";

    uint64_t start;
    uint64_t gap = largest_gap(&g, &start);

    if (gap < ALIGN_SECTORS)
        return "no free space on the disk";

    uint64_t count = gap;

    if (want && want < count)
        count = want & ~(uint64_t)(ALIGN_SECTORS - 1u);

    if (count < ALIGN_SECTORS)
        return "requested size too small";

    unsigned int slot = g.entry_count;

    for (unsigned int i = 0; i < g.entry_count; i++) {
        if (is_zero(&entries[i * g.entry_size], 16)) {
            slot = i;
            break;
        }
    }

    if (slot == g.entry_count)
        return "partition table is full";

    uint8_t *e = &entries[slot * g.entry_size];

    memset(e, 0, g.entry_size);
    memcpy(e, type_guid, 16);
    random_guid(&e[16]);
    wr64(&e[32], start);
    wr64(&e[40], start + count - 1u);

    for (unsigned int i = 0; i < 36 && name[i]; i++) {
        e[56 + i * 2] = (uint8_t)name[i];
        e[57 + i * 2] = 0;
    }

    const char *err = store(disk, &g);

    if (err)
        return err;

    *first_lba = start;
    *sector_count = count;
    return NULL;
}
