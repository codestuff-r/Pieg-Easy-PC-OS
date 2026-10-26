#include "libpieg.h"

const pieg_api_t *pieg;

#define NO_PATTERNS __attribute__((optimize("no-tree-loop-distribute-patterns")))

NO_PATTERNS void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    while (n--)
        *d++ = *s++;

    return dst;
}

NO_PATTERNS void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;

    if (d == s || n == 0)
        return dst;

    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }

    return dst;
}

NO_PATTERNS void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;

    while (n--)
        *d++ = (uint8_t)c;

    return dst;
}

NO_PATTERNS int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;

    while (n--) {
        if (*x != *y)
            return (int)*x - (int)*y;
        x++;
        y++;
    }

    return 0;
}

/* Entry point: the shell calls this with the kernel API table. */
__attribute__((visibility("default"), used))
int pieg_start(const pieg_api_t *api, int argc, char **argv)
{
    pieg = api;
    return main(argc, argv);
}

void p_putc(char c)
{
    pieg->putchar(c);
}

void p_puts(const char *s)
{
    pieg->write(s);
}

void p_putln(const char *s)
{
    pieg->write(s);
    pieg->putchar('\n');
}

void p_u64(uint64_t n)
{
    char buf[21];
    unsigned int count = 0;

    if (n == 0) {
        pieg->putchar('0');
        return;
    }

    while (n) {
        buf[count++] = (char)('0' + n % 10);
        n /= 10;
    }

    while (count)
        pieg->putchar(buf[--count]);
}

void p_hex(uint64_t n)
{
    static const char digits[] = "0123456789ABCDEF";
    int started = 0;

    pieg->write("0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int d = (unsigned int)((n >> shift) & 15);

        if (d || started || shift == 0) {
            pieg->putchar(digits[d]);
            started = 1;
        }
    }
}

void p_size(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        p_u64(bytes / (1024ull * 1024ull * 1024ull));
        pieg->write(" GiB");
    } else if (bytes >= 1024ull * 1024ull) {
        p_u64(bytes / (1024ull * 1024ull));
        pieg->write(" MiB");
    } else if (bytes >= 1024ull) {
        p_u64(bytes / 1024ull);
        pieg->write(" KiB");
    } else {
        p_u64(bytes);
        pieg->write(" bytes");
    }
}

char p_getc(void)
{
    for (;;) {
        char c = pieg->getchar();

        if (c)
            return c;
    }
}

size_t p_strlen(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

int p_streq(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++)
            return 0;
    }

    return *a == *b;
}

char p_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

int p_ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (p_lower(*a++) != p_lower(*b++))
            return 0;
    }

    return *a == *b;
}
