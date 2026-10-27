#ifndef PIEG_GPT_H
#define PIEG_GPT_H

#include <stdbool.h>
#include <stdint.h>

#include "blockdev.h"

/*
 * Small GPT editor, used to carve the persistent PIEG volume out of a
 * UEFI disk. It only ever touches (a) disks with a valid GPT, where it
 * adds one partition in free space, or (b) completely blank disks, where
 * it first writes an empty GPT. Anything else is left alone.
 */

typedef enum {
    GPT_STATE_OTHER = 0,    /* MBR, damaged GPT, data: do not touch */
    GPT_STATE_BLANK,        /* first and last sectors all zero */
    GPT_STATE_VALID         /* valid primary GPT */
} gpt_state_t;

/* Partition type GUID of a PIEG data volume (on-disk byte order). */
extern const uint8_t gpt_type_pieg[16];

/*
 * Classifies the disk. *largest_free gets the size in sectors of the
 * biggest 1 MiB-aligned gap a new partition could use (0 for OTHER).
 */
gpt_state_t gpt_inspect(blockdev_t *disk, uint64_t *largest_free);

/* Writes a protective MBR and an empty GPT. Blank disks only. */
const char *gpt_create(blockdev_t *disk);

/*
 * Adds a partition in the largest free gap. want == 0 takes the whole gap,
 * otherwise at most `want` sectors. Returns NULL on success or a message.
 */
const char *gpt_add_partition(blockdev_t *disk, uint64_t want,
                              const uint8_t type_guid[16], const char *name,
                              uint64_t *first_lba, uint64_t *sector_count);

#endif
