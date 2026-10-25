
#ifndef RAMSPEC_H
#define RAMSPEC_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    bool initialized;
    bool usage_available;
} ramspec_t;

bool ramspec_init(uint32_t multiboot_magic, uint32_t mbi_address);
bool get_ramspec(ramspec_t *spec);

#endif
