#ifndef LIBPIEG_H
#define LIBPIEG_H

/*
 * Tiny runtime for PIEG programs. Write `int main(int argc, char **argv)`,
 * put the file in programs/, run `make`. It is installed as
 * /PROGRAMS/<NAME>.ELF and runs by typing its name in the shell.
 */

#include <stddef.h>
#include <stdint.h>
#include "api.h"

extern const pieg_api_t *pieg;

int main(int argc, char **argv);

/* Output. */
void p_putc(char c);
void p_puts(const char *s);
void p_putln(const char *s);
void p_u64(uint64_t n);
void p_hex(uint64_t n);
void p_size(uint64_t bytes);

/* Input: blocks until a key is pressed. */
char p_getc(void);

/* Strings. */
size_t p_strlen(const char *s);
int p_streq(const char *a, const char *b);
int p_ieq(const char *a, const char *b);
char p_lower(char c);

/* The compiler emits calls to these. */
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

#endif
