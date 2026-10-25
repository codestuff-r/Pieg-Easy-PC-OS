#include <stdbool.h>
#include <stdint.h>
#include "panic.h"

#define VGA_MEMORY ((volatile uint16_t *)0xB8000)
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

#define VGA_COLOR(fg, bg) ((uint8_t)(((bg) << 4) | (fg)))

extern bool framebuffer_is_available(void);
extern void vga_raw_put(uint32_t col, uint32_t row, char c, uint8_t attr);
extern void vga_raw_clear(uint8_t attr);

/*
 * On UEFI machines there is no VGA text memory at 0xB8000, so panics are
 * drawn through the framebuffer when it is available and fall back to VGA
 * text memory otherwise (legacy BIOS).
 */
static void panic_put(unsigned int x, unsigned int y, char c, uint8_t color)
{
    if (framebuffer_is_available()) {
        vga_raw_put(x, y, c, color);
        return;
    }

    VGA_MEMORY[y * VGA_WIDTH + x] = ((uint16_t)color << 8) | (uint8_t)c;
}

static void panic_clear(uint8_t color)
{
    if (framebuffer_is_available()) {
        vga_raw_clear(color);
        return;
    }

    for (unsigned int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++)
        VGA_MEMORY[i] = ((uint16_t)color << 8) | ' ';
}

static void panic_write_at(
    unsigned int *x,
    unsigned int *y,
    const char *text,
    uint8_t color
)
{
    if (!text)
        return;

    while (*text) {
        char c = *text++;

        if (c == '\n') {
            *x = 0;
            (*y)++;
            continue;
        }

        if (*x >= VGA_WIDTH) {
            *x = 0;
            (*y)++;
        }

        if (*y >= VGA_HEIGHT)
            return;

        panic_put(*x, *y, c, color);

        (*x)++;
    }
}

__attribute__((noreturn))
static void panic_halt(void)
{
    __asm__ volatile("cli");

    for (;;) {
        __asm__ volatile("hlt");
    }
}

__attribute__((noreturn))
void kernel_panic(const char *reason, const char *details)
{
    unsigned int x = 2;
    unsigned int y = 2;

    const uint8_t background = VGA_COLOR(15, 4);
    const uint8_t heading = VGA_COLOR(15, 1);
    const uint8_t text = VGA_COLOR(15, 4);

    panic_clear(background);

    panic_write_at(&x, &y, "PIEG KERNEL PANIC", heading);

    x = 2;
    y = 4;
    panic_write_at(&x, &y, "The kernel cannot safely continue.", text);

    x = 2;
    y = 6;
    panic_write_at(&x, &y, "Reason: ", text);
    panic_write_at(&x, &y, reason ? reason : "Unknown", text);

    x = 2;
    y = 8;
    panic_write_at(&x, &y, "Details: ", text);
    panic_write_at(&x, &y, details ? details : "None", text);

    x = 2;
    y = 12;
    panic_write_at(&x, &y, "The CPU has been halted.", text);

    x = 2;
    y = 14;
    panic_write_at(
        &x,
        &y,
        "If this was intentional, the panic test worked.",
        text
    );

    panic_halt();
}

__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t error_code)
{
    (void)vector;
    (void)error_code;

    kernel_panic(
        "CPU exception",
        "The exception handler reported a processor fault."
    );
}
