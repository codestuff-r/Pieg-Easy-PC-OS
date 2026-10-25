#include "framebuffer.h"

#include <stddef.h>
#include <stdint.h>

#define MULTIBOOT2_BOOT_MAGIC 0x36D76289u
#define MULTIBOOT2_TAG_END 0
#define MULTIBOOT2_TAG_FRAMEBUFFER 8
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

static volatile uint8_t *fb;
static uint32_t fb_width;
static uint32_t fb_height;
static uint32_t fb_pitch;
static uint8_t fb_bpp;
static uint8_t red_position;
static uint8_t green_position;
static uint8_t blue_position;
static bool fb_ready;

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
    uint8_t red = (uint8_t)((color >> 16) & 0xFF);
    uint8_t green = (uint8_t)((color >> 8) & 0xFF);
    uint8_t blue = (uint8_t)(color & 0xFF);

    return channel_value(red, red_position, 8) |
           channel_value(green, green_position, 8) |
           channel_value(blue, blue_position, 8);
}

bool framebuffer_init(uint32_t magic, uint32_t info_address)
{
    fb_ready = false;
    fb = NULL;

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

            fb_ready = true;
            fb_dbg('F');
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

void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (!fb_ready || x >= fb_width || y >= fb_height)
        return;

    uint32_t pixel = convert_color(color);
    uint32_t bytes_per_pixel = fb_bpp / 8u;
    volatile uint8_t *destination =
        fb + (uint64_t)y * fb_pitch + (uint64_t)x * bytes_per_pixel;

    destination[0] = (uint8_t)pixel;
    destination[1] = (uint8_t)(pixel >> 8);
    destination[2] = (uint8_t)(pixel >> 16);

    if (bytes_per_pixel == 4)
        destination[3] = (uint8_t)(pixel >> 24);
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

    uint32_t pixel = convert_color(color);

    if (fb_bpp == 32) {
        for (uint32_t row = 0; row < height; row++) {
            volatile uint32_t *destination =
                (volatile uint32_t *)(fb + (uint64_t)(y + row) * fb_pitch
                                      + (uint64_t)x * 4u);

            for (uint32_t column = 0; column < width; column++)
                destination[column] = pixel;
        }
        return;
    }

    /* 24 bits per pixel: three bytes per pixel, no alignment assumptions. */
    uint8_t b0 = (uint8_t)pixel;
    uint8_t b1 = (uint8_t)(pixel >> 8);
    uint8_t b2 = (uint8_t)(pixel >> 16);

    for (uint32_t row = 0; row < height; row++) {
        volatile uint8_t *destination =
            fb + (uint64_t)(y + row) * fb_pitch + (uint64_t)x * 3u;

        for (uint32_t column = 0; column < width; column++) {
            destination[0] = b0;
            destination[1] = b1;
            destination[2] = b2;
            destination += 3;
        }
    }
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
