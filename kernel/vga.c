#include <stddef.h>
#include <stdint.h>
#include "framebuffer.h"

#define VGA_WIDTH  80
#define VGA_HEIGHT 25

static size_t cursor_row;
static size_t cursor_col;
static uint8_t text_color = 0x07;

static char text_buffer[VGA_HEIGHT][VGA_WIDTH];

static const uint8_t palette[16][3] = {
    { 0,   0,   0   }, { 0,   0,   170 },
    { 0,   170, 0   }, { 0,   170, 170 },
    { 170, 0,   0   }, { 170, 0,   170 },
    { 170, 85,  0   }, { 170, 170, 170 },
    { 85,  85,  85  }, { 85,  85,  255 },
    { 85,  255, 85  }, { 85,  255, 255 },
    { 255, 85,  85  }, { 255, 85,  255 },
    { 255, 255, 85  }, { 255, 255, 255 }
};

/*
 * Compact 5x7 font.
 * Each glyph uses five columns, with bit 0 at the top.
 * ASCII characters 32 through 126 are supported.
 */
static const uint8_t font5x7[95][5] = {
    {0,0,0,0,0}, {0,0,0x5F,0,0}, {0,7,0,7,0},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,8,0x64,0x62}, {0x36,0x49,0x55,0x22,0x50},
    {0,5,3,0,0}, {0,0x1C,0x22,0x41,0},
    {0,0x41,0x22,0x1C,0}, {0x14,8,0x3E,8,0x14},
    {8,8,0x3E,8,8}, {0,0x50,0x30,0,0},
    {8,8,8,8,8}, {0,0x60,0x60,0,0},
    {0x20,0x10,8,4,2},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0,0x42,0x7F,0x40,0},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {1,0x71,9,5,3},
    {0x36,0x49,0x49,0x49,0x36},
    {6,0x49,0x49,0x29,0x1E},
    {0,0x36,0x36,0,0}, {0,0x56,0x36,0,0},
    {8,0x14,0x22,0x41,0}, {0x14,0x14,0x14,0x14,0x14},
    {0,0x41,0x22,0x14,8}, {2,1,0x51,9,6},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,9,9,9,1}, {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,8,8,8,0x7F}, {0,0x41,0x7F,0x41,0},
    {0x20,0x40,0x41,0x3F,1},
    {0x7F,8,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,2,0x0C,2,0x7F}, {0x7F,4,8,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,9,9,9,6}, {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,9,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {1,1,0x7F,1,1}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,8,0x14,0x63},
    {7,8,0x70,8,7}, {0x61,0x51,0x49,0x45,0x43},
    {0,0x7F,0x41,0x41,0}, {2,4,8,0x10,0x20},
    {0,0x41,0x41,0x7F,0}, {4,2,1,2,4},
    {0x40,0x40,0x40,0x40,0x40},
    {0,1,2,4,0}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},
    {8,0x7E,9,1,2}, {0x0C,0x52,0x52,0x52,0x3E},
    {0x7F,8,4,4,0x78}, {0,0x44,0x7D,0x40,0},
    {0x20,0x40,0x44,0x3D,0},
    {0x7F,0x10,0x28,0x44,0},
    {0,0x41,0x7F,0x40,0},
    {0x7C,4,0x18,4,0x78},
    {0x7C,8,4,4,0x78},
    {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,8},
    {8,0x14,0x14,0x18,0x7C},
    {0x7C,8,4,4,8},
    {0x48,0x54,0x54,0x54,0x20},
    {4,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},
    {0,8,0x36,0x41,0},
    {0,0,0x7F,0,0}, {0,0x41,0x36,8,0},
    {8,4,8,0x10,8}
};

static uint32_t color_rgb(uint8_t index)
{
    index &= 0x0F;
    return ((uint32_t)palette[index][0] << 16) |
           ((uint32_t)palette[index][1] << 8) |
           (uint32_t)palette[index][2];
}

static uint32_t cell_width(void)
{
    uint32_t w = framebuffer_width() / VGA_WIDTH;
    return w ? w : 1;
}

static uint32_t cell_height(void)
{
    uint32_t h = framebuffer_height() / VGA_HEIGHT;
    return h ? h : 1;
}

/*
 * Draws one character cell. The 5x7 glyph is scaled in whole blocks
 * (bw x bh pixels per font pixel) so a cell costs at most ~36 rectangle
 * fills instead of one call per screen pixel.
 */
static void render_cell(size_t row, size_t col, uint8_t chval, uint8_t attr)
{
    if (!framebuffer_is_available() ||
        row >= VGA_HEIGHT || col >= VGA_WIDTH)
        return;

    uint32_t cw = cell_width();
    uint32_t ch = cell_height();
    uint32_t x0 = (uint32_t)col * cw;
    uint32_t y0 = (uint32_t)row * ch;

    if (chval < 32 || chval > 126)
        chval = '?';

    uint32_t foreground = color_rgb(attr & 0x0F);
    uint32_t background = color_rgb((attr >> 4) & 0x0F);

    framebuffer_fill_rect(x0, y0, cw, ch, background);

    if (chval == ' ')
        return;

    uint32_t bw = cw / 6;           /* 5 columns + spacing */
    uint32_t bh = ch / 8;           /* 7 rows + spacing */
    if (bw == 0) bw = 1;
    if (bh == 0) bh = 1;

    uint32_t ox = (cw > 5 * bw) ? (cw - 5 * bw) / 2 : 0;
    uint32_t oy = (ch > 7 * bh) ? (ch - 7 * bh) / 2 : 0;

    const uint8_t *glyph = font5x7[chval - 32];

    for (uint32_t gx = 0; gx < 5; gx++) {
        for (uint32_t gy = 0; gy < 7; gy++) {
            if (glyph[gx] & (1u << gy))
                framebuffer_fill_rect(x0 + ox + gx * bw,
                                      y0 + oy + gy * bh,
                                      bw, bh, foreground);
        }
    }
}

static void draw_cell(size_t row, size_t col)
{
    if (row >= VGA_HEIGHT || col >= VGA_WIDTH)
        return;

    render_cell(row, col, (uint8_t)text_buffer[row][col], text_color);
}

/* Used by panic.c: draw with an explicit attribute, bypassing the text buffer. */
void vga_raw_put(uint32_t col, uint32_t row, char c, uint8_t attr)
{
    render_cell(row, col, (uint8_t)c, attr);
}

void vga_raw_clear(uint8_t attr)
{
    if (framebuffer_is_available())
        framebuffer_clear(color_rgb((attr >> 4) & 0x0F));
}

static void redraw_screen(void)
{
    if (!framebuffer_is_available())
        return;

    framebuffer_clear(color_rgb((text_color >> 4) & 0x0F));

    for (size_t row = 0; row < VGA_HEIGHT; row++)
        for (size_t col = 0; col < VGA_WIDTH; col++)
            draw_cell(row, col);
}

static void scroll_if_needed(void)
{
    if (cursor_row < VGA_HEIGHT)
        return;

    for (size_t row = 1; row < VGA_HEIGHT; row++)
        for (size_t col = 0; col < VGA_WIDTH; col++)
            text_buffer[row - 1][col] = text_buffer[row][col];

    for (size_t col = 0; col < VGA_WIDTH; col++)
        text_buffer[VGA_HEIGHT - 1][col] = ' ';

    cursor_row = VGA_HEIGHT - 1;
    redraw_screen();
}

void vga_putchar(char c)
{
    if (!framebuffer_is_available())
        return;

    if (c == '\n') {
        cursor_col = 0;
        cursor_row++;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }

        text_buffer[cursor_row][cursor_col] = ' ';
        draw_cell(cursor_row, cursor_col);
    } else if (c == '\t') {
        size_t spaces = 4 - (cursor_col % 4);

        while (spaces--) {
            text_buffer[cursor_row][cursor_col] = ' ';
            draw_cell(cursor_row, cursor_col);

            if (++cursor_col >= VGA_WIDTH) {
                cursor_col = 0;
                cursor_row++;
                scroll_if_needed();
            }
        }
    } else {
        text_buffer[cursor_row][cursor_col] = c;
        draw_cell(cursor_row, cursor_col);

        if (++cursor_col >= VGA_WIDTH) {
            cursor_col = 0;
            cursor_row++;
        }
    }

    scroll_if_needed();
}

void vga_putc(char c)
{
    vga_putchar(c);
}

void vga_write_char(char c)
{
    vga_putchar(c);
}

void vga_write(const char *s)
{
    if (!s)
        return;

    while (*s)
        vga_putchar(*s++);
}

void vga_print(const char *s)
{
    vga_write(s);
}

void vga_backspace(void)
{
    vga_putchar('\b');
}

void vga_clear(void)
{
    cursor_row = 0;
    cursor_col = 0;

    for (size_t row = 0; row < VGA_HEIGHT; row++)
        for (size_t col = 0; col < VGA_WIDTH; col++)
            text_buffer[row][col] = ' ';

    redraw_screen();
}

void vga_set_color(uint8_t foreground, uint8_t background)
{
    text_color = (uint8_t)(((background & 0x0F) << 4) |
                           (foreground & 0x0F));
}

void vga_init(void)
{
    vga_clear();
}
