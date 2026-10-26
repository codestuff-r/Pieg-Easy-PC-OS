#ifndef PIEG_SHELLFS_H
#define PIEG_SHELLFS_H

#include <stdbool.h>

/* Called once when the shell starts: sets cwd to /U/HOME, prints status. */
void shellfs_start(void);

const char *shellfs_cwd(void);

/*
 * Runs a filesystem command (ls, cd, cat, mkdir, ...). Returns false when
 * argv[0] is not one of them so the shell can try its own commands.
 */
bool shellfs_command(int argc, char **argv);

#endif
