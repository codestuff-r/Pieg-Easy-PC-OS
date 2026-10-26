#include <stddef.h>
#include <stdint.h>
#include "framebuffer.h"

#define VGA_WIDTH       80
#define VGA_HEIGHT      25
#define SCROLLBACK_LINES 512
#define MAX_CELL_WIDTH  256

/*
 * Logical rows are stored in a ring. The visible screen is only a viewport
 * into this buffer, so old output remains available while scrolling.
 *
 * Drawing is incremental: a character repaints only its own cell, a new
 * line at the bottom scrolls the framebuffer by one cell row, and a full
 * repaint happens only when the user has scrolled the view back.
 */
static size_t cursor_row;
static size_t cursor_col;
static size_t view_offset;
static uint8_t text_color = 0x07;

static char text_buffer[SCROLLBACK_LINES][VGA_WIDTH];

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

    if (w == 0)
        w = 1;
    if (w > MAX_CELL_WIDTH)
        w = MAX_CELL_WIDTH;

    return w;
}

static uint32_t cell_height(void)
{
    uint32_t h = framebuffer_height() / VGA_HEIGHT;
    return h ? h : 1;
}

/*
 * Draws one character cell. Each glyph row is built once into a pixel line
 * and blitted bh times, so every pixel of the cell is written exactly once.
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

    uint32_t bg_rgb = color_rgb((attr >> 4) & 0x0F);

    if (chval == ' ') {
        framebuffer_fill_rect(x0, y0, cw, ch, bg_rgb);
        return;
    }

    uint32_t foreground = framebuffer_pack_color(color_rgb(attr & 0x0F));
    uint32_t background = framebuffer_pack_color(bg_rgb);

    uint32_t bw = cw / 6;           /* 5 columns + spacing */
    uint32_t bh = ch / 8;           /* 7 rows + spacing */
    if (bw == 0) bw = 1;
    if (bh == 0) bh = 1;

    uint32_t ox = (cw > 5 * bw) ? (cw - 5 * bw) / 2 : 0;
    uint32_t oy = (ch > 7 * bh) ? (ch - 7 * bh) / 2 : 0;

    const uint8_t *glyph = font5x7[chval - 32];
    uint32_t line[MAX_CELL_WIDTH];

    if (oy)
        framebuffer_fill_rect(x0, y0, cw, oy < ch ? oy : ch, bg_rgb);

    uint32_t y = oy;

    for (uint32_t gy = 0; gy < 7 && y < ch; gy++) {
        for (uint32_t i = 0; i < cw; i++)
            line[i] = background;

        for (uint32_t gx = 0; gx < 5; gx++) {
            if (!(glyph[gx] & (1u << gy)))
                continue;

            for (uint32_t k = 0; k < bw; k++) {
                uint32_t px = ox + gx * bw + k;

                if (px < cw)
                    line[px] = foreground;
            }
        }

        for (uint32_t k = 0; k < bh && y < ch; k++, y++)
            framebuffer_write_row(x0, y0 + y, line, cw);
    }

    if (y < ch)
        framebuffer_fill_rect(x0, y0 + y, cw, ch - y, bg_rgb);
}

static size_t oldest_row(void)
{
    return cursor_row >= SCROLLBACK_LINES - 1
        ? cursor_row - (SCROLLBACK_LINES - 1)
        : 0;
}

static size_t visible_top_row(void)
{
    size_t top = cursor_row >= VGA_HEIGHT - 1
        ? cursor_row - (VGA_HEIGHT - 1)
        : 0;
    size_t oldest = oldest_row();

    if (view_offset > top)
        return oldest;

    size_t visible_top = top - view_offset;
    return visible_top < oldest ? oldest : visible_top;
}

static void draw_cell(size_t logical_row, size_t col)
{
    if (col >= VGA_WIDTH)
        return;

    size_t top = visible_top_row();
    if (logical_row < top || logical_row >= top + VGA_HEIGHT)
        return;

    size_t screen_row = logical_row - top;
    render_cell(screen_row, col,
                (uint8_t)text_buffer[logical_row % SCROLLBACK_LINES][col],
                text_color);
}

/* Used by panic.c: draw with an explicit attribute, bypassing the text buffer. */
void vga_raw_put(uint32_t col, uint32_t row, char c, uint8_t attr)
{
    render_cell(row, col, (uint8_t)c, attr);
    framebuffer_flush();            /* the panic path must show immediately */
}

void vga_raw_clear(uint8_t attr)
{
    if (framebuffer_is_available()) {
        framebuffer_clear(color_rgb((attr >> 4) & 0x0F));
        framebuffer_flush();
    }
}

/* Repaints the viewport. Runs of blanks are filled as single rectangles. */
static void redraw_screen(void)
{
    if (!framebuffer_is_available())
        return;

    uint32_t cw = cell_width();
    uint32_t ch = cell_height();
    uint32_t bg = color_rgb((text_color >> 4) & 0x0F);
    size_t top = visible_top_row();
    size_t oldest = oldest_row();

    for (size_t screen_row = 0; screen_row < VGA_HEIGHT; ++screen_row) {
        size_t logical_row = top + screen_row;
        const char *line = 0;

        if (logical_row >= oldest && logical_row <= cursor_row)
            line = text_buffer[logical_row % SCROLLBACK_LINES];

        size_t col = 0;

        while (col < VGA_WIDTH) {
            if (!line || line[col] == ' ') {
                size_t start = col;

                while (col < VGA_WIDTH && (!line || line[col] == ' '))
                    col++;

                framebuffer_fill_rect((uint32_t)start * cw,
                                      (uint32_t)screen_row * ch,
                                      (uint32_t)(col - start) * cw,
                                      ch, bg);
            } else {
                render_cell(screen_row, col, (uint8_t)line[col],
                            text_color);
                col++;
            }
        }
    }
}

static void scroll_if_needed(void)
{
    /* Keep the cursor row valid in the circular history. */
    if (cursor_row >= SCROLLBACK_LINES) {
        size_t top = cursor_row >= VGA_HEIGHT - 1
            ? cursor_row - (VGA_HEIGHT - 1)
            : 0;
        size_t oldest = oldest_row();
        size_t maximum_offset = top > oldest ? top - oldest : 0;
        if (view_offset > maximum_offset)
            view_offset = maximum_offset;
    }
}

/* Shifts the live screen up one cell row; the new bottom row is blank. */
static void scroll_screen_one_line(void)
{
    uint32_t ch = cell_height();

    framebuffer_copy_rows(0, ch, (VGA_HEIGHT - 1) * ch);
    framebuffer_fill_rect(0, (VGA_HEIGHT - 1) * ch, framebuffer_width(), ch,
                          color_rgb((text_color >> 4) & 0x0F));
}

static void new_line(bool *full)
{
    cursor_col = 0;
    cursor_row++;

    for (size_t col = 0; col < VGA_WIDTH; ++col)
        text_buffer[cursor_row % SCROLLBACK_LINES][col] = ' ';

    if (*full)
        return;

    if (cursor_row >= VGA_HEIGHT)
        scroll_screen_one_line();
}

/*
 * Draws into the back buffer only. Nothing reaches video memory until
 * framebuffer_flush(): vga_write() flushes once per string, the keyboard poll
 * flushes whenever it finds no key waiting, and scrolling/clearing flush.
 */
static void put_one(char c)
{
    if (!framebuffer_is_available())
        return;

    bool full = view_offset != 0;

    if (c == '\n') {
        new_line(&full);
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        size_t top_before = visible_top_row();

        if (cursor_col > 0) {
            cursor_col--;
        } else if (cursor_row > 0) {
            cursor_row--;
            cursor_col = VGA_WIDTH - 1;
        }

        text_buffer[cursor_row % SCROLLBACK_LINES][cursor_col] = ' ';

        if (!full) {
            if (visible_top_row() != top_before)
                full = true;
            else
                draw_cell(cursor_row, cursor_col);
        }
    } else if (c == '\t') {
        size_t spaces = 4 - (cursor_col % 4);

        while (spaces--) {
            text_buffer[cursor_row % SCROLLBACK_LINES][cursor_col] = ' ';

            if (!full)
                draw_cell(cursor_row, cursor_col);

            if (++cursor_col >= VGA_WIDTH)
                new_line(&full);
        }
    } else {
        text_buffer[cursor_row % SCROLLBACK_LINES][cursor_col] = c;

        if (!full)
            draw_cell(cursor_row, cursor_col);

        if (++cursor_col >= VGA_WIDTH)
            new_line(&full);
    }

    scroll_if_needed();

    if (full)
        redraw_screen();
}

void vga_putchar(char c)
{
    put_one(c);
}

/*
 * direction < 0: scroll up; direction > 0: scroll down; direction == 0:
 * return to the live bottom so keyboard input appears beside the prompt.
 */
void vga_scroll_view(int direction)
{
    if (direction == 0 && view_offset == 0)
        return;

    size_t top = cursor_row >= VGA_HEIGHT - 1
        ? cursor_row - (VGA_HEIGHT - 1)
        : 0;
    size_t oldest = oldest_row();
    size_t maximum_offset = top > oldest ? top - oldest : 0;

    if (direction < 0) {
        if (view_offset < maximum_offset) {
            size_t amount = (size_t)(-direction);
            view_offset = view_offset + amount > maximum_offset
                ? maximum_offset : view_offset + amount;
        }
    } else if (direction > 0) {
        size_t amount = (size_t)direction;
        view_offset = view_offset > amount ? view_offset - amount : 0;
    } else {
        view_offset = 0;
    }

    redraw_screen();
    framebuffer_flush();
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
        put_one(*s++);

    framebuffer_flush();
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
    view_offset = 0;

    for (size_t row = 0; row < SCROLLBACK_LINES; ++row)
        for (size_t col = 0; col < VGA_WIDTH; ++col)
            text_buffer[row][col] = ' ';

    /* Clears the margins outside the 80x25 cell grid as well. The buffer is
     * all blanks, so a separate repaint of the grid would only redo this. */
    if (framebuffer_is_available()) {
        framebuffer_clear(color_rgb((text_color >> 4) & 0x0F));
        framebuffer_flush();
    }
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
