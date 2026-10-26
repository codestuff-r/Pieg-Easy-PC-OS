#include "partition.h"
#include "mem.h"

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool part_read(blockdev_t *dev, uint64_t lba, uint32_t count,
                      void *buffer)
{
    partition_t *p = (partition_t *)dev->driver_data;

    return blockdev_read(p->parent, p->start + lba, count, buffer);
}

static bool part_write(blockdev_t *dev, uint64_t lba, uint32_t count,
                       const void *buffer)
{
    partition_t *p = (partition_t *)dev->driver_data;

    return blockdev_write(p->parent, p->start + lba, count, buffer);
}

static bool add_partition(partition_t *out, unsigned int *count,
                          unsigned int max, blockdev_t *parent,
                          uint64_t start, uint64_t sectors)
{
    if (*count >= max || sectors == 0 ||
        start >= parent->sector_count ||
        sectors > parent->sector_count - start)
        return false;

    partition_t *p = &out[*count];
    unsigned int n = 0;

    memset(p, 0, sizeof(*p));

    while (parent->name[n] && n < sizeof(p->name) - 4) {
        p->name[n] = parent->name[n];
        n++;
    }

    p->name[n++] = 'p';

    if (*count + 1 >= 10)
        p->name[n++] = (char)('0' + (*count + 1) / 10);

    p->name[n++] = (char)('0' + (*count + 1) % 10);
    p->name[n] = '\0';

    p->parent = parent;
    p->start = start;

    p->dev.name = p->name;
    p->dev.sector_count = sectors;
    p->dev.sector_size = parent->sector_size;
    p->dev.driver_data = p;
    p->dev.read = part_read;
    p->dev.write = parent->write ? part_write : 0;

    (*count)++;
    return true;
}

static void scan_gpt(blockdev_t *parent, partition_t *out,
                     unsigned int *count, unsigned int max)
{
    uint8_t b[512];

    if (!blockdev_read(parent, 1, 1, b))
        return;

    static const char sig[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };

    if (memcmp(b, sig, 8) != 0)
        return;

    uint64_t entries_lba = rd64(&b[72]);
    uint32_t entries = rd32(&b[80]);
    uint32_t size = rd32(&b[84]);

    if (entries == 0 || (size != 128 && size != 256 && size != 512))
        return;

    if (entries > 128)
        entries = 128;

    uint64_t loaded = ~0ull;

    for (uint32_t i = 0; i < entries; i++) {
        uint64_t byte = (uint64_t)i * size;
        uint64_t lba = entries_lba + byte / 512;

        if (lba != loaded) {
            if (!blockdev_read(parent, lba, 1, b))
                return;
            loaded = lba;
        }

        const uint8_t *e = &b[byte % 512];
        bool used = false;

        for (unsigned int k = 0; k < 16; k++) {
            if (e[k]) {
                used = true;
                break;
            }
        }

        if (!used)
            continue;

        uint64_t first = rd64(&e[32]);
        uint64_t last = rd64(&e[40]);

        if (last < first)
            continue;

        add_partition(out, count, max, parent, first, last - first + 1);
    }
}

unsigned int partition_scan(blockdev_t *parent, partition_t *out,
                            unsigned int max)
{
    uint8_t b[512];
    unsigned int count = 0;
    bool gpt = false;

    if (!parent || !out || parent->sector_size != 512)
        return 0;

    if (!blockdev_read(parent, 0, 1, b))
        return 0;

    if (b[510] != 0x55 || b[511] != 0xAA)
        return 0;

    for (unsigned int i = 0; i < 4; i++) {
        const uint8_t *e = &b[446 + i * 16];
        uint8_t type = e[4];

        if (type == 0x00)
            continue;

        if (type == 0xEE) {
            gpt = true;
            continue;
        }

        /* Extended partitions are not walked. */
        if (type == 0x05 || type == 0x0F || type == 0x85)
            continue;

        add_partition(out, &count, max, parent,
                      rd32(&e[8]), rd32(&e[12]));
    }

    if (gpt)
        scan_gpt(parent, out, &count, max);

    return count;
}
