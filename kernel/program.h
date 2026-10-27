#ifndef PIEG_PROGRAM_H
#define PIEG_PROGRAM_H

#include <stdbool.h>

#define PIEG_SHELL_VERSION "0.7"

/*
 * Runs argv[0] as a program from /PROGRAMS (or by path when it contains a
 * '/'; ".ELF" is appended when needed). Returns false when no such program
 * exists. Load errors are printed here; *status gets the exit code.
 */
bool program_run(int argc, char **argv, int *status);

#endif
