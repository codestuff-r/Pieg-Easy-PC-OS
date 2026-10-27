#include <stddef.h>
#include <stdint.h>

#include "mkfat.h"
#include "mem.h"

#define BPS 512u

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Cluster size from the volume size (same table Microsoft uses for FAT32). */
static uint32_t pick_spc(uint64_t sectors)
{
    if (sectors <= 532480u)      return 1;      /* up to 260 MiB: 512 B */
    if (sectors <= 16777216u)    return 8;      /* up to 8 GiB: 4 KiB */
    if (sectors <= 33554432u)    return 16;
    if (sectors <= 67108864u)    return 32;
    return 64;
}

static const char *zero_sectors(blockdev_t *dev, uint64_t lba, uint32_t count)
{
    static uint8_t zeros[32 * BPS];     /* BSS: already zero */

    while (count) {
        uint32_t n = count > 32 ? 32 : count;

        if (!blockdev_write(dev, lba, n, zeros))
            return "disk write failed while clearing the volume";

        lba += n;
        count -= n;
    }

    return NULL;
}

const char *fat_format32(blockdev_t *dev, const char *label, uint32_t hidden)
{
    uint8_t s[BPS];
    const char *err;

    if (!dev || !dev->write || dev->sector_size != BPS)
        return "device is not writable";

    if (dev->sector_count < MKFAT_MIN_SECTORS)
        return "volume too small (64 MiB minimum)";

    if (dev->sector_count > 0xFFFFFFFFull)
        return "volume too large (2 TiB maximum)";

    uint32_t total = (uint32_t)dev->sector_count;
    uint32_t spc = pick_spc(total);
    uint32_t reserved = 32;
    uint32_t fats = 2;

    uint32_t tmp1 = total - reserved;
    uint32_t tmp2 = (256u * spc + fats) / 2u;
    uint32_t spf = (tmp1 + tmp2 - 1u) / tmp2;
    uint32_t clusters = (total - reserved - fats * spf) / spc;

    if (clusters < 65525u)
        return "volume too small for FAT32";

    uint32_t volume_id;
    uint32_t lo, hi;

    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    volume_id = lo ^ (hi * 2654435761u);

    /* ---- boot sector ---- */
    memset(s, 0, sizeof(s));
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    memcpy(&s[3], "PIEG    ", 8);
    put16(&s[11], BPS);
    s[13] = (uint8_t)spc;
    put16(&s[14], (uint16_t)reserved);
    s[16] = (uint8_t)fats;
    put16(&s[17], 0);               /* root entries: 0 on FAT32 */
    put16(&s[19], 0);
    s[21] = 0xF8;                   /* fixed disk */
    put16(&s[22], 0);
    put16(&s[24], 63);
    put16(&s[26], 255);
    put32(&s[28], hidden);
    put32(&s[32], total);
    put32(&s[36], spf);
    put16(&s[40], 0);
    put16(&s[42], 0);
    put32(&s[44], 2);               /* root directory cluster */
    put16(&s[48], 1);               /* FSInfo sector */
    put16(&s[50], 6);               /* backup boot sector */
    s[64] = 0x80;
    s[66] = 0x29;
    put32(&s[67], volume_id);

    memset(&s[71], ' ', 11);
    for (unsigned int i = 0; i < 11 && label && label[i]; i++) {
        char c = label[i];

        s[71 + i] = (uint8_t)((c >= 'a' && c <= 'z') ? c - 32 : c);
    }

    memcpy(&s[82], "FAT32   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;

    /* ---- FSInfo: counts unknown, the driver scans the FAT itself ---- */
    uint8_t info[BPS];

    memset(info, 0, sizeof(info));
    put32(&info[0], 0x41615252u);
    put32(&info[484], 0x61417272u);
    put32(&info[488], 0xFFFFFFFFu);
    put32(&info[492], 0xFFFFFFFFu);
    put32(&info[508], 0xAA550000u);

    /* Clear everything from the reserved area to the end of root cluster. */
    uint32_t data_start = reserved + fats * spf;

    err = zero_sectors(dev, 0, data_start + spc);
    if (err)
        return err;

    if (!blockdev_write(dev, 0, 1, s) ||
        !blockdev_write(dev, 1, 1, info) ||
        !blockdev_write(dev, 6, 1, s) ||
        !blockdev_write(dev, 7, 1, info))
        return "disk write failed (boot sector)";

    /* ---- FATs: media entry, end marker, root directory cluster ---- */
    uint8_t fat[BPS];

    memset(fat, 0, sizeof(fat));
    put32(&fat[0], 0x0FFFFFF8u);
    put32(&fat[4], 0x0FFFFFFFu);
    put32(&fat[8], 0x0FFFFFF8u);

    for (uint32_t f = 0; f < fats; f++) {
        if (!blockdev_write(dev, reserved + f * spf, 1, fat))
            return "disk write failed (FAT)";
    }

    return NULL;
}
