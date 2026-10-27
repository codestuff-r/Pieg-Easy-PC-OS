#ifndef PIEG_PERSIST_H
#define PIEG_PERSIST_H

#include <stdint.h>

/*
 * Persistent storage setup. Carves a "PIEG" FAT32 volume out of free space
 * on a GPT disk (a UEFI-style disk), copies the running system onto it and
 * leaves it for fsinit to find on the next boot.
 */

typedef struct {
    char name[16];
    uint64_t sectors;
    int state;                  /* 0 = unsupported, 1 = blank, 2 = valid GPT */
    uint64_t free_sectors;      /* usable for a new volume */
} persist_disk_t;

unsigned int persist_disk_count(void);
int persist_disk_info(unsigned int index, persist_disk_t *info);

/*
 * Creates the volume. size_mib == 0 means "all free space, at most 4 GiB".
 * Returns NULL on success, otherwise a message describing what failed.
 */
const char *persist_install(unsigned int index, uint32_t size_mib);

#endif
