#ifndef PIEG_MKFAT_H
#define PIEG_MKFAT_H

#include <stdint.h>

#include "blockdev.h"

/* Smallest volume fat_format32() accepts (FAT32 needs >= 65525 clusters). */
#define MKFAT_MIN_SECTORS (64u * 2048u)     /* 64 MiB */

/*
 * Writes an empty FAT32 filesystem over the whole block device.
 * `label` is up to 11 characters, `hidden` is the device's first LBA on
 * its disk (goes into the BPB). Returns NULL on success, else a message.
 */
const char *fat_format32(blockdev_t *dev, const char *label, uint32_t hidden);

#endif
