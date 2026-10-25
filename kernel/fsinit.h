
#ifndef PIEG_FSINIT_H
#define PIEG_FSINIT_H

#include <stdbool.h>
#include <stdint.h>

struct fs_dir_entry_unused;

void fsinit(void);
bool fs_is_mounted(void);
uint64_t fs_disk_sectors(void);
const char *fs_type_name(void);

typedef void (*fs_list_callback_t)(
    const char *name,
    uint8_t attributes,
    uint32_t size,
    void *context
);

bool fs_list_root(fs_list_callback_t callback, void *context);

#endif
