#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "shellfs.h"
#include "fat.h"
#include "fsinit.h"

extern void vga_putchar(char c);
extern void vga_write(const char *s);

static char cwd[FAT_PATH_MAX] = "/";

static void put(const char *s)
{
    vga_write(s);
}

const char *shellfs_cwd(void)
{
    return cwd;
}

void shellfs_cd(const char *arg)
{
    char path[FAT_PATH_MAX];
    bool directory;
    fat_err_t r;

    fs_resolve(cwd, arg ? arg : "~", path, sizeof(path));

    r = fs_stat(path, &directory, 0);

    if (r == FAT_OK && !directory)
        r = FAT_ERR_NOTDIR;

    if (r != FAT_OK) {
        put("cd: ");
        put(path);
        put(": ");
        put(fat_error_string(r));
        vga_putchar('\n');
        return;
    }

    for (size_t i = 0; i < sizeof(cwd); i++) {
        cwd[i] = path[i];

        if (!path[i])
            break;
    }
}

void shellfs_start(void)
{
    if (!fs_is_mounted()) {
        put("No filesystem mounted (no FAT volume found).\n");
        return;
    }

    put("Filesystem: ");
    put(fs_type_name());
    put(" on ");
    put(fs_device_name());
    put(fs_is_writable() ? " (read/write)" : " (read-only)");

    if (fs_is_pieg_volume())
        put("  [PIEG volume: /#  /PROGRAMS  /U/HOME]");

    vga_putchar('\n');

    if (fs_stat(PIEG_DIR_HOME, 0, 0) == FAT_OK) {
        for (size_t i = 0; PIEG_DIR_HOME[i]; i++) {
            cwd[i] = PIEG_DIR_HOME[i];
            cwd[i + 1] = '\0';
        }
    }
}
