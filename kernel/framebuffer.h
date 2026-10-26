#ifndef PIEG_FRAMEBUFFER_H
#define PIEG_FRAMEBUFFER_H

#include <stdbool.h>
#include <stdint.h>

#define PIEG_COLOR_BLACK  0x000000
#define PIEG_COLOR_WHITE  0xFFFFFF
#define PIEG_COLOR_BLUE   0x0078D4
#define PIEG_COLOR_GREEN  0x00FF00
#define PIEG_COLOR_RED    0xFF0000

bool framebuffer_init(uint32_t multiboot_magic, uint32_t multiboot_info);
bool framebuffer_is_available(void);

uint32_t framebuffer_width(void);
uint32_t framebuffer_height(void);
uint32_t framebuffer_pitch(void);

void framebuffer_clear(uint32_t color);
void framebuffer_putpixel(uint32_t x, uint32_t y, uint32_t color);
void framebuffer_fill_rect(
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    uint32_t color
);
void framebuffer_draw_line(
    int x0,
    int y0,
    int x1,
    int y1,
    uint32_t color
);

/* Converts 0xRRGGBB to the framebuffer's native pixel layout. */
uint32_t framebuffer_pack_color(uint32_t rgb);

/* Writes `count` native pixels (from framebuffer_pack_color) at (x, y). */
void framebuffer_write_row(
    uint32_t x,
    uint32_t y,
    const uint32_t *pixels,
    uint32_t count
);

/* Copies `rows` pixel rows from src_y to dst_y (dst_y < src_y). */
void framebuffer_copy_rows(uint32_t dst_y, uint32_t src_y, uint32_t rows);

/*
 * All drawing goes to a RAM back buffer; nothing is visible until this is
 * called. It copies only the changed area to video memory and is a cheap
 * no-op when nothing changed, so calling it often is fine.
 */
void framebuffer_flush(void);

#endif
