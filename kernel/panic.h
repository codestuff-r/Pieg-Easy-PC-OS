#ifndef PIEG_PANIC_H
#define PIEG_PANIC_H

#include <stdint.h>

__attribute__((noreturn))
void kernel_panic(const char *reason, const char *details);

void exception_init(void);

__attribute__((noreturn))
void exception_panic(uint64_t vector, uint64_t error_code);

#endif
