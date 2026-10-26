#include "libpieg.h"

/* Example program: shows argv and the working directory. */
int main(int argc, char **argv)
{
    p_putln("Hello from a PIEG program!");

    p_puts("Working directory: ");
    p_putln(pieg->cwd());

    for (int i = 1; i < argc; i++) {
        p_puts("arg ");
        p_u64((uint64_t)i);
        p_puts(": ");
        p_putln(argv[i]);
    }

    return 0;
}
