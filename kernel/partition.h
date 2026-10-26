#ifndef PIEG_PARTITION_H
#define PIEG_PARTITION_H

#include "blockdev.h"

#define PARTITION_MAX 8

typedef struct {
    blockdev_t dev;         /* must stay first: registered with blockdev */
    blockdev_t *parent;
    uint64_t start;
    char name[16];
} partition_t;

/*
 * Reads the MBR (or GPT behind a protective MBR) of `parent` and fills
 * `out` with up to `max` partitions exposed as block devices named
 * "<parent>p<N>". Returns the number found.
 */
unsigned int partition_scan(blockdev_t *parent, partition_t *out,
                            unsigned int max);

#endif
