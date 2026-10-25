#include "blockdev.h"

static blockdev_t *devices[BLOCKDEV_MAX_DEVICES];
static unsigned int device_count;

bool blockdev_register(blockdev_t *dev)
{
    if (!dev || !dev->read || dev->sector_size == 0)
        return false;

    if (device_count >= BLOCKDEV_MAX_DEVICES)
        return false;

    for (unsigned int i = 0; i < device_count; i++) {
        if (devices[i] == dev)
            return false;
    }

    devices[device_count++] = dev;
    return true;
}

blockdev_t *blockdev_get(unsigned int index)
{
    if (index >= device_count)
        return 0;

    return devices[index];
}

unsigned int blockdev_count(void)
{
    return device_count;
}

bool blockdev_read(blockdev_t *dev,
                   uint64_t lba,
                   uint32_t count,
                   void *buffer)
{
    if (!dev || !dev->read || !buffer || count == 0)
        return false;

    if (lba >= dev->sector_count ||
        count > dev->sector_count - lba)
        return false;

    return dev->read(dev, lba, count, buffer);
}

bool blockdev_write(blockdev_t *dev,
                    uint64_t lba,
                    uint32_t count,
                    const void *buffer)
{
    if (!dev || !dev->write || !buffer || count == 0)
        return false;

    if (lba >= dev->sector_count ||
        count > dev->sector_count - lba)
        return false;

    return dev->write(dev, lba, count, buffer);
}
