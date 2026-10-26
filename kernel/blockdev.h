#ifndef PIEG_BLOCKDEV_H
#define PIEG_BLOCKDEV_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define BLOCKDEV_MAX_DEVICES 16
#define BLOCKDEV_SECTOR_SIZE 512

typedef struct blockdev {
    const char *name;
    uint64_t sector_count;
    uint32_t sector_size;
    void *driver_data;

    bool (*read)(struct blockdev *dev,
                 uint64_t lba,
                 uint32_t count,
                 void *buffer);

    bool (*write)(struct blockdev *dev,
                  uint64_t lba,
                  uint32_t count,
                  const void *buffer);
} blockdev_t;

bool blockdev_register(blockdev_t *dev);
blockdev_t *blockdev_get(unsigned int index);
unsigned int blockdev_count(void);

bool blockdev_read(blockdev_t *dev,
                   uint64_t lba,
                   uint32_t count,
                   void *buffer);

bool blockdev_write(blockdev_t *dev,
                    uint64_t lba,
                    uint32_t count,
                    const void *buffer);

#endif
