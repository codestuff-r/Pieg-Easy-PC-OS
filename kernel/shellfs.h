#ifndef PIEG_SHELLFS_H
#define PIEG_SHELLFS_H

/* Called once when the shell starts: sets cwd to /U/HOME, prints status. */
void shellfs_start(void);

const char *shellfs_cwd(void);

/* The `cd` built-in: changes the shell's working directory. */
void shellfs_cd(const char *arg);

#endif
