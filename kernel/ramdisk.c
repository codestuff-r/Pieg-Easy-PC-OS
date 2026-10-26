#include "ramdisk.h"
#include "blockdev.h"
#include "mem.h"

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END    0u
#define MULTIBOOT2_TAG_MODULE 3u
#define MULTIBOOT2_MAX_INFO_SIZE (16u * 1024u * 1024u)

extern void vga_write(const char *text);

typedef struct {
    uint32_t type;
    uint32_t size;
} __attribute__((packed)) mb2_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
} __attribute__((packed)) mb2_module_tag_t;

static blockdev_t devices[RAMDISK_MAX];
static char names[RAMDISK_MAX][4];

static bool ramdisk_read(blockdev_t *dev, uint64_t lba, uint32_t count,
                         void *buffer)
{
    const uint8_t *base = (const uint8_t *)dev->driver_data;

    memcpy(buffer, base + lba * 512u, (size_t)count * 512u);
    return true;
}

static bool ramdisk_write(blockdev_t *dev, uint64_t lba, uint32_t count,
                          const void *buffer)
{
    uint8_t *base = (uint8_t *)dev->driver_data;

    memcpy(base + lba * 512u, buffer, (size_t)count * 512u);
    return true;
}

static void print_u32(uint32_t value)
{
    char text[11];
    unsigned int n = 0;

    if (value == 0) {
        vga_write("0");
        return;
    }

    while (value) {
        text[n++] = (char)('0' + value % 10);
        value /= 10;
    }

    while (n) {
        char one[2] = { text[--n], '\0' };
        vga_write(one);
    }
}

unsigned int ramdisk_init(uint32_t magic, uint32_t mbi)
{
    unsigned int count = 0;

    if (magic != MULTIBOOT2_BOOT_MAGIC || mbi == 0)
        return 0;

    const uint8_t *info = (const uint8_t *)(uintptr_t)mbi;
    uint32_t total = *(const uint32_t *)info;

    if (total < 16 || total > MULTIBOOT2_MAX_INFO_SIZE)
        return 0;

    uint32_t offset = 8;

    while (offset <= total - 8) {
        const mb2_tag_t *tag = (const mb2_tag_t *)(info + offset);

        if (tag->size < 8 || tag->size > total - offset)
            return count;

        if (tag->type == MULTIBOOT2_TAG_END)
            break;

        if (tag->type == MULTIBOOT2_TAG_MODULE &&
            tag->size >= sizeof(mb2_module_tag_t) &&
            count < RAMDISK_MAX) {
            const mb2_module_tag_t *m = (const mb2_module_tag_t *)tag;
            uint32_t bytes = m->mod_end > m->mod_start
                           ? m->mod_end - m->mod_start : 0;

            if (bytes >= 512) {
                blockdev_t *dev = &devices[count];

                names[count][0] = 'r';
                names[count][1] = 'd';
                names[count][2] = (char)('0' + count);
                names[count][3] = '\0';

                dev->name = names[count];
                dev->sector_count = bytes / 512u;
                dev->sector_size = 512;
                dev->driver_data = (void *)(uintptr_t)m->mod_start;
                dev->read = ramdisk_read;
                dev->write = ramdisk_write;

                if (blockdev_register(dev)) {
                    vga_write("[RAMDISK] ");
                    vga_write(dev->name);
                    vga_write(": boot module, ");
                    print_u32(bytes / 1024u);
                    vga_write(" KiB\n");
                    count++;
                }
            }
        }

        offset = (offset + tag->size + 7u) & ~7u;
    }

    return count;
}
