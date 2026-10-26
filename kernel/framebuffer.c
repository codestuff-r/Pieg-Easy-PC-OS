#include "framebuffer.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_MODULE 3
#define MULTIBOOT2_TAG_MMAP 6
#define MULTIBOOT2_TAG_FRAMEBUFFER 8
#define MULTIBOOT2_MEMORY_AVAILABLE 1
#define MULTIBOOT2_FRAMEBUFFER_RGB 1

#define MAX_FRAMEBUFFER_DIMENSION 16384u

typedef struct {
    uint32_t type;
    uint32_t size;
} multiboot_tag_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint64_t address;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t framebuffer_type;
    uint16_t reserved;
} multiboot_framebuffer_tag_t;

typedef struct {
    uint8_t red_position;
    uint8_t red_size;
    uint8_t green_position;
    uint8_t green_size;
    uint8_t blue_position;
    uint8_t blue_size;
} multiboot_rgb_info_t;

static inline void fb_dbg(char c)
{
    __asm__ volatile ("outb %0, $0xE9" : : "a"(c));
}


static void fb_dbg_str(const char *s)
{
    while (*s)
        fb_dbg(*s++);
}

static void fb_dbg_hex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int started = 0;

    fb_dbg('0');
    fb_dbg('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int d = (unsigned int)((v >> shift) & 0xF);
        if (d || started || shift == 0) {
            fb_dbg(digits[d]);
            started = 1;
        }
    }
}

/* Prints every field GRUB reported, so a rejection can be diagnosed. */
static void fb_dump(const multiboot_framebuffer_tag_t *t,
                    const multiboot_rgb_info_t *rgb)
{
    fb_dbg_str("\nFB addr=");   fb_dbg_hex(t->address);
    fb_dbg_str(" pitch=");       fb_dbg_hex(t->pitch);
    fb_dbg_str(" w=");           fb_dbg_hex(t->width);
    fb_dbg_str(" h=");           fb_dbg_hex(t->height);
    fb_dbg_str(" bpp=");         fb_dbg_hex(t->bpp);
    fb_dbg_str(" type=");        fb_dbg_hex(t->framebuffer_type);
    fb_dbg_str(" R=");           fb_dbg_hex(rgb->red_position);
    fb_dbg('/');                 fb_dbg_hex(rgb->red_size);
    fb_dbg_str(" G=");           fb_dbg_hex(rgb->green_position);
    fb_dbg('/');                 fb_dbg_hex(rgb->green_size);
    fb_dbg_str(" B=");           fb_dbg_hex(rgb->blue_position);
    fb_dbg('/');                 fb_dbg_hex(rgb->blue_size);
    fb_dbg('\n');
}

/*
 * Rendering model
 * ---------------
 * All drawing goes into a RAM shadow copy of the screen (same pitch, same
 * pixel format). framebuffer_flush() then pushes only the changed rectangle to
 * video memory with large sequential writes. Video memory is never read back:
 * reads from MMIO are what made scrolling crawl on real hardware.
 *
 * If no RAM can be found for the shadow, drawing falls back to writing the
 * screen directly (slower, still correct).
 */
static volatile uint8_t *fb;        /* the real screen (MMIO)            */
static uint8_t *draw;               /* where drawing goes: shadow or fb  */
static bool buffered;               /* draw != fb                        */
static uint64_t fb_bytes;           /* pitch * height                    */
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint8_t fb_bpp;
static uint8_t red_position;
static uint8_t green_position;
static uint8_t blue_position;
static bool fb_ready;

/* Dirty rectangle, x in BYTES within a row, y in rows. */
static bool dirty;
static uint32_t dirty_bx0, dirty_bx1, dirty_y0, dirty_y1;

/* One-entry cache: text rendering converts the same few colors constantly. */
static uint32_t cached_rgb;
static uint32_t cached_pixel;
static bool cached_valid;

static uint32_t channel_value(uint8_t channel, uint8_t position,
                              uint8_t size)
{
    if (size == 0 || size > 8 || position >= 32 ||
        (uint32_t)position + size > 32) {
        return 0;
    }

    uint32_t maximum = (1u << size) - 1u;
    uint32_t scaled = ((uint32_t)channel * maximum + 127u) / 255u;

    return scaled << position;
}

static uint32_t convert_color(uint32_t color)
{
    if (cached_valid && cached_rgb == color)
        return cached_pixel;

    uint8_t red = (uint8_t)((color >> 16) & 0xFF);
    uint8_t green = (uint8_t)((color >> 8) & 0xFF);
    uint8_t blue = (uint8_t)(color & 0xFF);

    uint32_t pixel = channel_value(red, red_position, 8) |
                     channel_value(green, green_position, 8) |
                     channel_value(blue, blue_position, 8);

    cached_rgb = color;
    cached_pixel = pixel;
    cached_valid = true;

    return pixel;
}

static inline void store32(void *dst, uint32_t pixel, uint64_t count)
{
    __asm__ volatile ("rep stosl"
                      : "+D"(dst), "+c"(count)
                      : "a"(pixel)
                      : "memory");
}

/* Forward copy: qwords first, then the tail. Safe when dst < src. */
static inline void copy_bytes(void *dst, const void *src, uint64_t n)
{
    uint64_t q = n >> 3;
    uint64_t r = n & 7u;

    __asm__ volatile ("rep movsq"
                      : "+D"(dst), "+S"(src), "+c"(q)
                      :
                      : "memory");
    __asm__ volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(r)
                      :
                      : "memory");
}

static inline void mark_dirty(uint32_t x, uint32_t y,
                              uint32_t w, uint32_t h)
{
    if (!buffered)
        return;

    uint32_t bpp = fb_bpp / 8u;
    uint32_t bx0 = x * bpp;
    uint32_t bx1 = (x + w) * bpp;

    if (!dirty) {
        dirty = true;
        dirty_bx0 = bx0;
        dirty_bx1 = bx1;
        dirty_y0 = y;
        dirty_y1 = y + h;
        return;
    }

    if (bx0 < dirty_bx0) dirty_bx0 = bx0;
    if (bx1 > dirty_bx1) dirty_bx1 = bx1;
    if (y < dirty_y0) dirty_y0 = y;
    if (y + h > dirty_y1) dirty_y1 = y + h;
}

/* ---------------------------------------------------------------------------
 * Write-combining for the framebuffer
 *
 * Page tables from entry.asm map 0..4 GiB with 2 MiB write-back pages, so the
 * effective type of video memory is whatever the firmware's MTRRs say, which
 * on many laptops is uncached: every store is a separate bus transaction.
 * Reprogramming PAT entry 4 as write-combining and setting the PAT bit on the
 * 2 MiB pages that cover the framebuffer lets the CPU merge stores into full
 * bursts.
 * ------------------------------------------------------------------------ */
extern uint64_t pd_table[];

#define MSR_IA32_PAT 0x277u

static void fb_enable_write_combining(uint64_t address, uint64_t bytes)
{
    uint32_t eax = 1, ebx, ecx = 0, edx;

    __asm__ volatile ("cpuid"
                      : "+a"(eax), "=b"(ebx), "+c"(ecx), "=d"(edx));

    if (!(edx & (1u << 16))) {          /* no PAT */
        fb_dbg('w');
        return;
    }

    /* PA0 WB, PA1 WT, PA2 UC-, PA3 UC, PA4 WC, PA5 WT, PA6 UC-, PA7 UC. */
    uint32_t lo = 0x00070406u;
    uint32_t hi = 0x00070401u;

    __asm__ volatile ("wrmsr" : : "c"(MSR_IA32_PAT), "a"(lo), "d"(hi));

    uint64_t first = address >> 21;
    uint64_t last = (address + bytes - 1u) >> 21;

    for (uint64_t i = first; i <= last && i < 2048u; i++)
        pd_table[i] |= (1ull << 12);    /* PAT bit of a 2 MiB page -> index 4 */

    uint64_t cr3;

    __asm__ volatile ("wbinvd" : : : "memory");
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");

    fb_dbg('W');
}

/* ---------------------------------------------------------------------------
 * Finding RAM for the shadow buffer
 *
 * No allocator exists yet, so pick a free region out of the Multiboot2 memory
 * map that overlaps nothing in use: low memory, the kernel image (including
 * .bss), the Multiboot2 info block and every boot module. Must lie below
 * 4 GiB, which is all entry.asm identity-maps.
 * ------------------------------------------------------------------------ */
#define MAX_RESERVED 24

typedef struct {
    uint64_t start;
    uint64_t end;
} range_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
} multiboot_mmap_tag_t;

typedef struct {
    uint64_t address;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} multiboot_mmap_entry_t;

typedef struct {
    uint32_t type;
    uint32_t size;
    uint32_t mod_start;
    uint32_t mod_end;
} multiboot_module_tag_t;

extern char _end[];

static range_t reserved_ranges[MAX_RESERVED];
static unsigned int reserved_count;
static bool reserved_overflow;

static void reserve(uint64_t start, uint64_t end)
{
    if (reserved_count >= MAX_RESERVED) {
        reserved_overflow = true;
        return;
    }

    reserved_ranges[reserved_count].start = start;
    reserved_ranges[reserved_count].end = end;
    reserved_count++;
}

static uint64_t align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1u) & ~(a - 1u);
}

static uint8_t *find_shadow_region(const uint8_t *info, uint32_t total_size,
                                   uint64_t need)
{
    reserved_count = 0;
    reserved_overflow = false;

    reserve(0, 0x100000u);
    reserve(0x100000u, align_up((uint64_t)(uintptr_t)_end, 4096u));
    reserve((uint64_t)(uintptr_t)info,
            align_up((uint64_t)(uintptr_t)info + total_size, 4096u));

    for (uint32_t off = 8; off <= total_size - 8; ) {
        const multiboot_tag_t *tag = (const multiboot_tag_t *)(info + off);

        if (tag->size < 8 || tag->size > total_size - off ||
            tag->type == MULTIBOOT2_TAG_END)
            break;

        if (tag->type == MULTIBOOT2_TAG_MODULE &&
            tag->size >= sizeof(multiboot_module_tag_t)) {
            const multiboot_module_tag_t *m =
                (const multiboot_module_tag_t *)tag;

            reserve(m->mod_start, align_up(m->mod_end, 4096u));
        }

        off = (off + tag->size + 7u) & ~7u;
    }

    if (reserved_overflow)
        return NULL;

    for (uint32_t off = 8; off <= total_size - 8; ) {
        const multiboot_tag_t *tag = (const multiboot_tag_t *)(info + off);

        if (tag->size < 8 || tag->size > total_size - off ||
            tag->type == MULTIBOOT2_TAG_END)
            break;

        if (tag->type == MULTIBOOT2_TAG_MMAP &&
            tag->size >= sizeof(multiboot_mmap_tag_t)) {
            const multiboot_mmap_tag_t *mm =
                (const multiboot_mmap_tag_t *)tag;

            if (mm->entry_size < sizeof(multiboot_mmap_entry_t))
                return NULL;

            for (uint32_t pos = sizeof(multiboot_mmap_tag_t);
                 pos + mm->entry_size <= tag->size;
                 pos += mm->entry_size) {
                const multiboot_mmap_entry_t *e =
                    (const multiboot_mmap_entry_t *)((const uint8_t *)tag + pos);

                if (e->type != MULTIBOOT2_MEMORY_AVAILABLE || e->length == 0)
                    continue;

                uint64_t end = e->address + e->length;
                uint64_t s = align_up(e->address, 4096u);

                if (end > 0x100000000ULL)
                    end = 0x100000000ULL;

                while (s + need <= end) {
                    bool hit = false;

                    for (unsigned int i = 0; i < reserved_count; i++) {
                        if (s < reserved_ranges[i].end &&
                            s + need > reserved_ranges[i].start) {
                            s = align_up(reserved_ranges[i].end, 4096u);
                            hit = true;
                            break;
                        }
                    }

                    if (!hit)
                        return (uint8_t *)(uintptr_t)s;
                }
            }
        }

        off = (off + tag->size + 7u) & ~7u;
    }

    return NULL;
}

bool framebuffer_init(uint32_t magic, uint32_t info_address)
{
    fb_ready = false;
    fb = NULL;
    draw = NULL;
    buffered = false;
    dirty = false;
    cached_valid = false;

    if (magic != MULTIBOOT2_BOOT_MAGIC || info_address == 0)
        return false;

    const uint8_t *info = (const uint8_t *)(uintptr_t)info_address;
    uint32_t total_size = *(const uint32_t *)info;

    if (total_size < 16 || total_size > 16u * 1024u * 1024u)
        return false;

    uint32_t offset = 8;

    while (offset <= total_size - 8) {
        const multiboot_tag_t *tag =
            (const multiboot_tag_t *)(info + offset);

        if (tag->size < 8 || tag->size > total_size - offset)
            return false;

        if (tag->type == MULTIBOOT2_TAG_END)
            break;

        if (tag->type == MULTIBOOT2_TAG_FRAMEBUFFER &&
            tag->size >= sizeof(multiboot_framebuffer_tag_t) +
                         sizeof(multiboot_rgb_info_t)) {

            const multiboot_framebuffer_tag_t *framebuffer_tag =
                (const multiboot_framebuffer_tag_t *)tag;

            const multiboot_rgb_info_t *rgb =
                (const multiboot_rgb_info_t *)
                ((const uint8_t *)tag +
                 sizeof(multiboot_framebuffer_tag_t));

            uint64_t address = framebuffer_tag->address;
            uint64_t bytes =
                (uint64_t)framebuffer_tag->pitch *
                framebuffer_tag->height;

            if (framebuffer_tag->framebuffer_type !=
                    MULTIBOOT2_FRAMEBUFFER_RGB ||
                (framebuffer_tag->bpp != 32 && framebuffer_tag->bpp != 24) ||
                framebuffer_tag->width == 0 ||
                framebuffer_tag->height == 0 ||
                framebuffer_tag->width > MAX_FRAMEBUFFER_DIMENSION ||
                framebuffer_tag->height > MAX_FRAMEBUFFER_DIMENSION ||
                framebuffer_tag->pitch <
                    framebuffer_tag->width * (framebuffer_tag->bpp / 8u) ||
                bytes == 0 ||
                address + bytes < address ||
                address + bytes > 0x100000000ULL ||
                rgb->red_size != 8 ||
                rgb->green_size != 8 ||
                rgb->blue_size != 8) {
                fb_dbg('f');
                fb_dump(framebuffer_tag, rgb);
                return false;
            }

            fb = (volatile uint8_t *)(uintptr_t)address;
            fb_width = framebuffer_tag->width;
            fb_height = framebuffer_tag->height;
            fb_pitch = framebuffer_tag->pitch;
            fb_bpp = framebuffer_tag->bpp;

            red_position = rgb->red_position;
            green_position = rgb->green_position;
            blue_position = rgb->blue_position;

            if ((uint32_t)red_position + 8 > 32 ||
                (uint32_t)green_position + 8 > 32 ||
                (uint32_t)blue_position + 8 > 32) {
                fb = NULL;
                return false;
            }

            fb_bytes = bytes;
            draw = (uint8_t *)fb;

            uint8_t *shadow = find_shadow_region(info, total_size,
                                                 align_up(bytes, 4096u));

            if (shadow) {
                draw = shadow;
                buffered = true;
            }

            fb_ready = true;
            fb_dbg('F');                /* test.sh matches EKF* */
            fb_dbg(buffered ? 'S' : 's');   /* s = direct drawing, slow */

            fb_enable_write_combining(address, bytes);   /* W, or w if no PAT */
            return true;
        }

        offset = (offset + tag->size + 7u) & ~7u;
    }

    fb_dbg('n');   /* no usable framebuffer tag */
    return false;
}

bool framebuffer_is_available(void)
{
    return fb_ready;
}

uint32_t framebuffer_width(void)
{
    return fb_ready ? fb_width : 0;
}

uint32_t framebuffer_height(void)
{
    return fb_ready ? fb_height : 0;
}

uint32_t framebuffer_pitch(void)
{
    return fb_ready ? fb_pitch : 0;
}

uint32_t framebuffer_pack_color(uint32_t rgb)
{
    return convert_color(rgb);
}

void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!fb_ready || x >= fb_width || y >= fb_height)
        return;

    uint32_t pixel = convert_color(color);
    uint32_t bytes_per_pixel = fb_bpp / 8u;
    uint8_t *destination =
        draw + (uint64_t)y * fb_pitch + (uint64_t)x * bytes_per_pixel;

    destination[0] = (uint8_t)pixel;
    destination[1] = (uint8_t)(pixel >> 8);
    destination[2] = (uint8_t)(pixel >> 16);

    if (bytes_per_pixel == 4)
        destination[3] = (uint8_t)(pixel >> 24);

    mark_dirty(x, y, 1, 1);
}

void framebuffer_clear(uint32_t color)
{
    framebuffer_fill_rect(0, 0, fb_width, fb_height, color);
}

void framebuffer_fill_rect(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color)
{
    if (!fb_ready || x >= fb_width || y >= fb_height)
        return;

    if (width > fb_width - x)
        width = fb_width - x;

    if (height > fb_height - y)
        height = fb_height - y;

    if (width == 0 || height == 0)
        return;

    uint32_t pixel = convert_color(color);

    mark_dirty(x, y, width, height);

    if (fb_bpp == 32) {
        /* Rows are contiguous when the rect spans the whole pitch. */
        if (x == 0 && width == fb_width && fb_pitch == fb_width * 4u) {
            store32(draw + (uint64_t)y * fb_pitch, pixel,
                    (uint64_t)width * height);
            return;
        }

        for (uint32_t row = 0; row < height; row++)
            store32(draw + (uint64_t)(y + row) * fb_pitch + (uint64_t)x * 4u,
                    pixel, width);
        return;
    }

    /* 24 bits per pixel: three bytes per pixel, no alignment assumptions. */
    uint8_t b0 = (uint8_t)pixel;
    uint8_t b1 = (uint8_t)(pixel >> 8);
    uint8_t b2 = (uint8_t)(pixel >> 16);

    for (uint32_t row = 0; row < height; row++) {
        uint8_t *destination =
            draw + (uint64_t)(y + row) * fb_pitch + (uint64_t)x * 3u;

        for (uint32_t column = 0; column < width; column++) {
            destination[0] = b0;
            destination[1] = b1;
            destination[2] = b2;
            destination += 3;
        }
    }
}

void framebuffer_write_row(
    uint32_t x,
    uint32_t y,
    const uint32_t *pixels,
    uint32_t count)
{
    if (!fb_ready || !pixels || x >= fb_width || y >= fb_height)
        return;

    if (count > fb_width - x)
        count = fb_width - x;

    mark_dirty(x, y, count, 1);

    if (fb_bpp == 32) {
        void *d = (void *)(draw + (uint64_t)y * fb_pitch + (uint64_t)x * 4u);
        const void *s = pixels;

        __asm__ volatile ("rep movsl"
                          : "+D"(d), "+S"(s), "+c"(count)
                          :
                          : "memory");
        return;
    }

    uint8_t *destination =
        draw + (uint64_t)y * fb_pitch + (uint64_t)x * 3u;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t pixel = pixels[i];

        destination[0] = (uint8_t)pixel;
        destination[1] = (uint8_t)(pixel >> 8);
        destination[2] = (uint8_t)(pixel >> 16);
        destination += 3;
    }
}

void framebuffer_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t rows)
{
    if (!fb_ready || dst_y >= src_y || src_y >= fb_height)
        return;

    if (rows > fb_height - src_y)
        rows = fb_height - src_y;

    if (rows == 0)
        return;

    uint32_t bytes = fb_width * (fb_bpp / 8u);

    /* Source and destination are in the RAM shadow: this never reads MMIO. */
    if (bytes == fb_pitch) {
        copy_bytes(draw + (uint64_t)dst_y * fb_pitch,
                   draw + (uint64_t)src_y * fb_pitch,
                   (uint64_t)rows * fb_pitch);
    } else {
        for (uint32_t row = 0; row < rows; row++)
            copy_bytes(draw + (uint64_t)(dst_y + row) * fb_pitch,
                       draw + (uint64_t)(src_y + row) * fb_pitch, bytes);
    }

    mark_dirty(0, dst_y, fb_width, rows);
}

/* Pushes everything drawn since the last flush to video memory. */
void framebuffer_flush(void)
{
    if (!buffered || !dirty)
        return;

    uint32_t bpp = fb_bpp / 8u;
    uint32_t y0 = dirty_y0;
    uint32_t y1 = dirty_y1 > fb_height ? fb_height : dirty_y1;
    uint32_t row_bytes = fb_width * bpp;
    uint32_t bx0 = dirty_bx0 & ~7u;             /* keep stores qword aligned */
    uint32_t bx1 = (dirty_bx1 + 7u) & ~7u;

    dirty = false;

    if (y0 >= y1)
        return;

    if (bx1 > fb_pitch)
        bx1 = fb_pitch;

    if (bx0 == 0 && dirty_bx1 >= row_bytes) {
        /* Full-width: one long sequential burst. */
        uint64_t offset = (uint64_t)y0 * fb_pitch;
        uint64_t length = (uint64_t)(y1 - y0) * fb_pitch;

        if (offset + length > fb_bytes)
            length = fb_bytes - offset;

        copy_bytes((void *)(fb + offset), draw + offset, length);
    } else {
        for (uint32_t row = y0; row < y1; row++) {
            uint64_t offset = (uint64_t)row * fb_pitch + bx0;

            copy_bytes((void *)(fb + offset), draw + offset, bx1 - bx0);
        }
    }

    __asm__ volatile ("sfence" : : : "memory");
}

void framebuffer_draw_line(
    int x0, int y0, int x1, int y1, uint32_t color)
{
    if (!fb_ready)
        return;

    int dx = x1 >= x0 ? x1 - x0 : x0 - x1;
    int sx = x0 < x1 ? 1 : -1;
    int dy = y1 >= y0 ? y0 - y1 : y1 - y0;
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    for (;;) {
        if (x0 >= 0 && y0 >= 0 &&
            (uint32_t)x0 < fb_width &&
            (uint32_t)y0 < fb_height) {
            framebuffer_putpixel((uint32_t)x0, (uint32_t)y0, color);
        }

        if (x0 == x1 && y0 == y1)
            break;

        int twice_error = 2 * error;

        if (twice_error >= dy) {
            error += dy;
            x0 += sx;
        }

        if (twice_error <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}
