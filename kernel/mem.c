#include <stddef.h>
#include <stdint.h>
#include "mem.h"

/*
 * Freestanding build: GCC emits calls to these for struct copies and
 * zero-initialisation. Loop-pattern recognition is disabled so the
 * bodies cannot be turned back into calls to themselves.
 */
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
