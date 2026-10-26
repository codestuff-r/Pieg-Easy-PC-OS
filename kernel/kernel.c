
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "framebuffer.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END       0u
#define MULTIBOOT2_TAG_MMAP      6u
#define MULTIBOOT2_MEMORY_AVAILABLE 1u
#define MULTIBOOT2_MAX_INFO_SIZE (16u * 1024u * 1024u)

extern void vga_init(void);
extern void vga_write(const char *text);
extern void driverinit(void);
extern void fsinit(uint32_t magic, uint32_t mbi);
extern void terminal_run(void);

/* RAM specification returned by get_ramspec(). */
typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    bool initialized;
    bool usage_available;
} ramspec_t;

/* Multiboot2 structures. */
struct __attribute__((packed)) multiboot2_info {
    uint32_t total_size;
    uint32_t reserved;
};

struct __attribute__((packed)) multiboot2_tag {
    uint32_t type;
    uint32_t size;
};

struct __attribute__((packed)) multiboot2_mmap_tag {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
};

struct __attribute__((packed)) multiboot2_mmap_entry {
    uint64_t address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
};

static ramspec_t system_ram;
static bool ram_initialized;

/*
 * Parse the Multiboot2 memory map and total the usable regions.
 * This reports usable physical memory, not current allocator usage.
 */
bool get_ramspec(ramspec_t *spec)
{
    if (spec == NULL)
        return false;

    spec->total_bytes = 0;
    spec->used_bytes = 0;
    spec->free_bytes = 0;
    spec->initialized = false;
    spec->usage_available = false;

    if (!ram_initialized)
        return false;

    *spec = system_ram;
    return true;
}

static bool init_ramspec(uint32_t magic, uint32_t mbi_address)
{
    ram_initialized = false;

    system_ram.total_bytes = 0;
    system_ram.used_bytes = 0;
    system_ram.free_bytes = 0;
    system_ram.initialized = false;
    system_ram.usage_available = false;

    if (magic != MULTIBOOT2_BOOT_MAGIC || mbi_address == 0)
        return false;

    const struct multiboot2_info *info =
        (const struct multiboot2_info *)(uintptr_t)mbi_address;

    uint32_t total_size = info->total_size;

    if (total_size < sizeof(struct multiboot2_info) + 8u ||
        total_size > MULTIBOOT2_MAX_INFO_SIZE)
        return false;

    uint32_t offset = 8u;
    bool found_end = false;
    bool found_mmap = false;

    while (offset <= total_size - 8u) {
        const struct multiboot2_tag *tag =
            (const struct multiboot2_tag *)
            ((const uint8_t *)info + offset);

        if (tag->size < 8u || tag->size > total_size - offset)
            return false;

        if (tag->type == MULTIBOOT2_TAG_END) {
            if (tag->size != 8u)
                return false;

            found_end = true;
            break;
        }

        if (tag->type == MULTIBOOT2_TAG_MMAP) {
            if (tag->size < sizeof(struct multiboot2_mmap_tag))
                return false;

            const struct multiboot2_mmap_tag *mmap =
                (const struct multiboot2_mmap_tag *)tag;

            if (mmap->entry_size <
                sizeof(struct multiboot2_mmap_entry))
                return false;

            uint32_t entries_size =
                tag->size - sizeof(struct multiboot2_mmap_tag);

            if (entries_size % mmap->entry_size != 0)
                return false;

            uint32_t pos = sizeof(struct multiboot2_mmap_tag);

            while (pos < tag->size) {
                const struct multiboot2_mmap_entry *entry =
                    (const struct multiboot2_mmap_entry *)
                    ((const uint8_t *)tag + pos);

                if (entry->type == MULTIBOOT2_MEMORY_AVAILABLE &&
                    entry->length != 0 &&
                    entry->address <= UINT64_MAX - entry->length) {
                    if (system_ram.total_bytes >
                        UINT64_MAX - entry->length)
                        system_ram.total_bytes = UINT64_MAX;
                    else
                        system_ram.total_bytes += entry->length;
                }

                pos += mmap->entry_size;
            }

            found_mmap = true;
        }

        uint32_t next_offset = (offset + tag->size + 7u) & ~7u;

        if (next_offset <= offset || next_offset > total_size)
            return false;

        offset = next_offset;
    }

    if (!found_end || !found_mmap || system_ram.total_bytes == 0)
        return false;

    system_ram.initialized = true;
    system_ram.usage_available = false;
    ram_initialized = true;

    return true;
}

static void draw_pieg_screen(void)
{
    uint32_t width = framebuffer_width();
    uint32_t height = framebuffer_height();

    if (width == 0 || height == 0)
        return;

    framebuffer_clear(0x071426);

    framebuffer_fill_rect(0, 0, width, 8, PIEG_COLOR_BLUE);

    uint32_t logo_x = width / 2u - 80u;
    uint32_t logo_y = height / 3u - 40u;

    framebuffer_fill_rect(logo_x, logo_y, 160, 80, 0x102D49);
    framebuffer_fill_rect(logo_x + 12, logo_y + 12, 136, 4, 0x00C8FF);
    framebuffer_fill_rect(logo_x + 12, logo_y + 24, 90, 4, 0xFFFFFF);
    framebuffer_fill_rect(logo_x + 12, logo_y + 36, 112, 4, 0xFFFFFF);
    framebuffer_fill_rect(logo_x + 12, logo_y + 48, 64, 4, 0x00C8FF);

    framebuffer_fill_rect(0, height - 8, width, 8, 0x102D49);
    framebuffer_fill_rect(12, height - 6, 48, 4, 0x00FF88);
}

void kernel_main(uint32_t magic, uint32_t mbi_address)
{
    bool have_framebuffer = false;

    if (magic == MULTIBOOT2_BOOT_MAGIC)
        have_framebuffer = framebuffer_init(magic, mbi_address);

    vga_init();

    if (magic != MULTIBOOT2_BOOT_MAGIC) {
        vga_write("PIEG: invalid Multiboot2 boot magic.\n");
        for (;;)
            __asm__ volatile ("cli; hlt");
    }

    if (have_framebuffer)
        draw_pieg_screen();
    else
        vga_write("PIEG: framebuffer unavailable.\n");

    vga_write("PIEG OS\n");
    vga_write("Pieg Isn't Even GNU\n");
    vga_write("Boot protocol: Multiboot2\n");
    vga_write("Architecture: x86-64\n");

    if (init_ramspec(magic, mbi_address)) {
        ramspec_t ram;

        if (get_ramspec(&ram)) {
            vga_write("RAM specification initialized.\n");
            vga_write("Usable physical RAM: available through get_ramspec().\n");
        }
    } else {
        vga_write("PIEG: unable to read Multiboot2 memory map.\n");
    }

    vga_write("Initializing hardware drivers...\n");

    driverinit();
    fsinit(magic, mbi_address);

    terminal_run();

    for (;;)
        __asm__ volatile ("cli; hlt");
}
