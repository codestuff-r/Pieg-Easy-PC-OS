
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "ramspec.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END    0
#define MULTIBOOT2_TAG_MMAP   6
#define MULTIBOOT2_MEMORY_AVAILABLE 1

#define MULTIBOOT2_MAX_INFO_SIZE (16u * 1024u * 1024u)
#define MULTIBOOT2_MMAP_MIN_SIZE  16u
#define MULTIBOOT2_ENTRY_MIN_SIZE 24u

typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_info_t;

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) multiboot2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} __attribute__((packed)) multiboot2_mmap_tag_t;

typedef struct {
    uint64_t address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} __attribute__((packed)) multiboot2_mmap_entry_t;

static ramspec_t ram_state;

static uint64_t add_saturating_u64(uint64_t a, uint64_t b)
{
    if (UINT64_MAX - a < b)
        return UINT64_MAX;

    return a + b;
}

bool ramspec_init(uint32_t multiboot_magic, uint32_t mbi_address)
{
    ram_state = (ramspec_t){0};

    if (multiboot_magic != MULTIBOOT2_BOOT_MAGIC || mbi_address == 0)
        return false;

    const multiboot2_info_t *info =
        (const multiboot2_info_t *)(uintptr_t)mbi_address;

    uint32_t total_size = info->total_size;

    if (total_size < sizeof(multiboot2_info_t) ||
        total_size > MULTIBOOT2_MAX_INFO_SIZE)
        return false;

    const uint8_t *base = (const uint8_t *)info;
    uint32_t offset = sizeof(multiboot2_info_t);
    bool found_mmap = false;

    while (offset <= total_size - sizeof(multiboot2_tag_t)) {
        const multiboot2_tag_t *tag =
            (const multiboot2_tag_t *)(base + offset);

        if (tag->size < sizeof(multiboot2_tag_t) ||
            tag->size > total_size - offset)
            return false;

        if (tag->type == MULTIBOOT2_TAG_END)
            break;

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            if (tag->size < sizeof(multiboot2_mmap_tag_t))
                return false;

            const multiboot2_mmap_tag_t *mmap =
                (const multiboot2_mmap_tag_t *)tag;

            if (mmap->entry_size < MULTIBOOT2_ENTRY_MIN_SIZE)
                return false;

            uint32_t entries_size =
                tag->size - sizeof(multiboot2_mmap_tag_t);

            if (entries_size % mmap->entry_size != 0)
                return false;

            const uint8_t *entry_data =
                (const uint8_t *)mmap +
                sizeof(multiboot2_mmap_tag_t);

            uint32_t entry_count = entries_size / mmap->entry_size;

            for (uint32_t i = 0; i < entry_count; i++) {
                const multiboot2_mmap_entry_t *entry =
                    (const multiboot2_mmap_entry_t *)
                    (entry_data + (size_t)i * mmap->entry_size);

                if (entry->type != MULTIBOOT2_MEMORY_AVAILABLE)
                    continue;

                /* Ignore zero-length or overflowing memory regions. */
                if (entry->length == 0 ||
                    UINT64_MAX - entry->address < entry->length)
                    continue;

                ram_state.total_bytes = add_saturating_u64(
                    ram_state.total_bytes,
                    entry->length
                );
            }

            found_mmap = true;
        }

        uint32_t next_offset =
            (offset + tag->size + 7u) & ~7u;

        if (next_offset <= offset || next_offset > total_size)
            break;

        offset = next_offset;
    }

    if (!found_mmap)
        return false;

    ram_state.initialized = true;

    /*
     * No physical memory allocator exists yet.
     * Do not pretend that used/free values are accurate.
     */
    ram_state.used_bytes = 0;
    ram_state.free_bytes = 0;
    ram_state.usage_available = false;

    return true;
}

bool get_ramspec(ramspec_t *spec)
{
    if (!spec || !ram_state.initialized)
        return false;

    *spec = ram_state;
    return true;
}
