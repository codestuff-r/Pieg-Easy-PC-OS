#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "shellfs.h"
#include "blockdev.h"
#include "fat.h"
#include "fsinit.h"

extern void vga_putchar(char c);
extern void vga_write(const char *s);

static char cwd[FAT_PATH_MAX] = "/";
static uint8_t io_buffer[4096];

/* ================================================================
 * Output and string helpers
 * ================================================================ */

static void put(const char *s)
{
    vga_write(s);
}

static void putln(const char *s)
{
    vga_write(s);
    vga_putchar('\n');
}

static void put_u64(uint64_t n)
{
    char buf[21];
    unsigned int count = 0;

    if (n == 0) {
        vga_putchar('0');
        return;
    }

    while (n) {
        buf[count++] = (char)('0' + n % 10);
        n /= 10;
    }

    while (count)
        vga_putchar(buf[--count]);
}

static void put_size(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        put_u64(bytes / (1024ull * 1024ull * 1024ull));
        put(" GiB");
    } else if (bytes >= 1024ull * 1024ull) {
        put_u64(bytes / (1024ull * 1024ull));
        put(" MiB");
    } else if (bytes >= 1024ull) {
        put_u64(bytes / 1024ull);
        put(" KiB");
    } else {
        put_u64(bytes);
        put(" bytes");
    }
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (lower(*a++) != lower(*b++))
            return false;
    }

    return *a == *b;
}

static size_t length(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

static void fail(const char *command, const char *path, fat_err_t error)
{
    put(command);
    put(": ");

    if (path) {
        put(path);
        put(": ");
    }

    putln(fat_error_string(error));
}

static void resolve(const char *arg, char *out)
{
    fs_resolve(cwd, arg, out, FAT_PATH_MAX);
}

static const char *base_name(const char *path)
{
    const char *base = path;

    for (const char *p = path; *p; p++) {
        if (*p == '/' && p[1])
            base = p + 1;
    }

    return base;
}

/* Joins argv[first..] with single spaces into buffer; returns length. */
static uint32_t join_args(int argc, char **argv, int first, char *buffer,
                          uint32_t capacity)
{
    uint32_t n = 0;

    for (int i = first; i < argc; i++) {
        if (i > first && n + 1 < capacity)
            buffer[n++] = ' ';

        for (const char *p = argv[i]; *p && n + 1 < capacity; p++)
            buffer[n++] = *p;
    }

    if (n + 1 < capacity)
        buffer[n++] = '\n';

    buffer[n] = '\0';
    return n;
}

/* ================================================================
 * Listing
 * ================================================================ */

typedef struct {
    uint64_t files;
    uint64_t directories;
    uint64_t bytes;
} list_stats_t;

static void list_entry(const char *name, uint8_t attributes, uint32_t size,
                       void *context)
{
    list_stats_t *stats = (list_stats_t *)context;
    bool directory = (attributes & FAT_ATTR_DIRECTORY) != 0;

    put(directory ? "[DIR]  " : "[FILE] ");
    put(name);

    if (directory) {
        stats->directories++;
    } else {
        put("  ");
        put_u64(size);
        put(" bytes");
        stats->files++;
        stats->bytes += size;
    }

    vga_putchar('\n');
}

static void cmd_ls(const char *command, int argc, char **argv)
{
    const char *target = ".";

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            target = argv[i];
            break;
        }
    }

    char path[FAT_PATH_MAX];
    bool directory;
    uint32_t size;

    resolve(target, path);

    fat_err_t r = fs_stat(path, &directory, &size);

    if (r != FAT_OK) {
        fail(command, path, r);
        return;
    }

    if (!directory) {
        put("[FILE] ");
        put(path);
        put("  ");
        put_u64(size);
        putln(" bytes");
        return;
    }

    list_stats_t stats = {0};

    put(path);
    putln(":");

    r = fs_list(path, list_entry, &stats);

    if (r != FAT_OK) {
        fail(command, path, r);
        return;
    }

    put("Files: ");
    put_u64(stats.files);
    put("  Directories: ");
    put_u64(stats.directories);
    put("  Bytes: ");
    put_u64(stats.bytes);
    vga_putchar('\n');
}

/* ================================================================
 * File commands
 * ================================================================ */

static void cmd_cd(int argc, char **argv)
{
    char path[FAT_PATH_MAX];
    bool directory;

    resolve(argc > 1 ? argv[1] : "~", path);

    fat_err_t r = fs_stat(path, &directory, 0);

    if (r != FAT_OK) {
        fail("cd", path, r);
        return;
    }

    if (!directory) {
        fail("cd", path, FAT_ERR_NOTDIR);
        return;
    }

    for (size_t i = 0; i < sizeof(cwd); i++) {
        cwd[i] = path[i];

        if (!path[i])
            break;
    }
}

static void cmd_cat(const char *command, int argc, char **argv)
{
    if (argc < 2) {
        put("Usage: ");
        put(command);
        putln(" <file>");
        return;
    }

    for (int i = 1; i < argc; i++) {
        char path[FAT_PATH_MAX];
        bool directory;
        uint32_t size;

        resolve(argv[i], path);

        fat_err_t r = fs_stat(path, &directory, &size);

        if (r == FAT_OK && directory)
            r = FAT_ERR_ISDIR;

        if (r != FAT_OK) {
            fail(command, path, r);
            continue;
        }

        uint32_t offset = 0;
        char last = '\n';

        while (offset < size) {
            uint32_t got = 0;

            r = fs_read(path, offset, io_buffer, 512, &got);

            if (r != FAT_OK || got == 0) {
                if (r != FAT_OK)
                    fail(command, path, r);
                break;
            }

            for (uint32_t k = 0; k < got; k++) {
                char c = (char)io_buffer[k];

                if (c == '\r')
                    continue;

                if (c != '\n' && c != '\t' && (c < 32 || c > 126))
                    c = '.';

                vga_putchar(c);
                last = c;
            }

            offset += got;
        }

        if (last != '\n')
            vga_putchar('\n');
    }
}

static void cmd_mkdir(int argc, char **argv)
{
    if (argc < 2) {
        putln("Usage: mkdir <directory>...");
        return;
    }

    for (int i = 1; i < argc; i++) {
        char path[FAT_PATH_MAX];

        resolve(argv[i], path);

        fat_err_t r = fs_mkdir(path);

        if (r != FAT_OK)
            fail("mkdir", path, r);
    }
}

static void cmd_touch(int argc, char **argv)
{
    if (argc < 2) {
        putln("Usage: touch <file>...");
        return;
    }

    for (int i = 1; i < argc; i++) {
        char path[FAT_PATH_MAX];

        resolve(argv[i], path);

        if (fs_stat(path, 0, 0) == FAT_OK)
            continue;

        fat_err_t r = fs_create(path);

        if (r != FAT_OK)
            fail("touch", path, r);
    }
}

static void cmd_rm(const char *command, int argc, char **argv)
{
    if (argc < 2) {
        put("Usage: ");
        put(command);
        putln(" <path>...");
        return;
    }

    for (int i = 1; i < argc; i++) {
        char path[FAT_PATH_MAX];

        resolve(argv[i], path);

        fat_err_t r = fs_remove(path);

        if (r != FAT_OK)
            fail(command, path, r);
    }
}

static void cmd_write(const char *command, int argc, char **argv,
                      bool append)
{
    if (argc < 3) {
        put("Usage: ");
        put(command);
        putln(" <file> <text...>");
        return;
    }

    char path[FAT_PATH_MAX];
    char text[512];

    resolve(argv[1], path);

    uint32_t n = join_args(argc, argv, 2, text, sizeof(text));
    fat_err_t r = fs_write(path, text, n, append);

    if (r != FAT_OK)
        fail(command, path, r);
}

/* echo text > file / echo text >> file */
static bool cmd_echo_redirect(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        bool append = ieq(argv[i], ">>");

        if (!append && !ieq(argv[i], ">"))
            continue;

        if (i + 1 >= argc) {
            putln("echo: missing file name after redirection");
            return true;
        }

        char path[FAT_PATH_MAX];
        char text[512];

        resolve(argv[i + 1], path);

        uint32_t n = join_args(i, argv, 1, text, sizeof(text));

        fat_err_t r = fs_write(path, text, n, append);

        if (r != FAT_OK)
            fail("echo", path, r);

        return true;
    }

    return false;
}

/* Copies one file; dst may be a directory. */
static fat_err_t copy_file(const char *src, const char *dst_arg,
                           char *final_dst)
{
    char dst[FAT_PATH_MAX];
    bool directory;
    uint32_t size;

    fat_err_t r = fs_stat(src, &directory, &size);

    if (r != FAT_OK)
        return r;

    if (directory)
        return FAT_ERR_ISDIR;

    resolve(dst_arg, dst);

    if (fs_stat(dst, &directory, 0) == FAT_OK && directory) {
        size_t n = length(dst);

        if (n + 1 + length(base_name(src)) + 1 > sizeof(dst))
            return FAT_ERR_NAME;

        if (n > 1)
            dst[n++] = '/';

        for (const char *p = base_name(src); *p; p++)
            dst[n++] = *p;

        dst[n] = '\0';
    }

    for (size_t i = 0; i < FAT_PATH_MAX; i++) {
        final_dst[i] = dst[i];

        if (!dst[i])
            break;
    }

    /* Creates or truncates the target, then streams the data in. */
    r = fs_write(dst, "", 0, false);

    if (r != FAT_OK)
        return r;

    uint32_t offset = 0;

    while (offset < size) {
        uint32_t got = 0;

        r = fs_read(src, offset, io_buffer, sizeof(io_buffer), &got);

        if (r != FAT_OK)
            return r;

        if (got == 0)
            break;

        r = fs_write(dst, io_buffer, got, true);

        if (r != FAT_OK)
            return r;

        offset += got;
    }

    return FAT_OK;
}

static void cmd_cp(const char *command, int argc, char **argv, bool move)
{
    if (argc != 3) {
        put("Usage: ");
        put(command);
        putln(" <source> <destination>");
        return;
    }

    char src[FAT_PATH_MAX];
    char dst[FAT_PATH_MAX];

    resolve(argv[1], src);

    fat_err_t r = copy_file(src, argv[2], dst);

    if (r != FAT_OK) {
        fail(command, src, r);
        return;
    }

    if (move) {
        r = fs_remove(src);

        if (r != FAT_OK)
            fail(command, src, r);
    }
}

/* ================================================================
 * Storage status
 * ================================================================ */

static void cmd_mount(void)
{
    putln("Block devices");

    for (unsigned int i = 0; i < blockdev_count(); i++) {
        blockdev_t *dev = blockdev_get(i);

        put("  ");
        put(dev->name);
        put("  ");
        put_size(dev->sector_count * dev->sector_size);
        put(dev->write ? "  rw" : "  ro");

        if (fs_is_mounted() && ieq(dev->name, fs_device_name()))
            put("  mounted on /");

        vga_putchar('\n');
    }

    if (!fs_is_mounted()) {
        putln("No filesystem mounted.");
        return;
    }

    put("Filesystem: ");
    put(fs_type_name());
    put("  label=");
    putln(fs_label()[0] ? fs_label() : "(none)");

    put("PIEG volume: ");
    putln(fs_is_pieg_volume() ? "yes" : "no");

    put("Access: ");
    putln(fs_is_writable() ? "read/write" : "read-only");
}

static void cmd_df(void)
{
    uint64_t total;
    uint64_t free_bytes;

    fat_err_t r = fs_df(&total, &free_bytes);

    if (r != FAT_OK) {
        fail("df", 0, r);
        return;
    }

    put("Filesystem: ");
    put(fs_type_name());
    put(" on ");
    putln(fs_device_name());

    put("Total: ");
    put_size(total);
    vga_putchar('\n');

    put("Used:  ");
    put_size(total - free_bytes);
    vga_putchar('\n');

    put("Free:  ");
    put_size(free_bytes);
    vga_putchar('\n');
}

/* ================================================================
 * Entry points
 * ================================================================ */

const char *shellfs_cwd(void)
{
    return cwd;
}

void shellfs_start(void)
{
    if (!fs_is_mounted()) {
        putln("No filesystem mounted (no FAT volume found).");
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

bool shellfs_command(int argc, char **argv)
{
    if (argc < 1)
        return false;

    const char *name = argv[0];

    if (ieq(name, "pwd")) {
        putln(cwd);
    } else if (ieq(name, "cd")) {
        cmd_cd(argc, argv);
    } else if (ieq(name, "ls") || ieq(name, "dir")) {
        cmd_ls(name, argc, argv);
    } else if (ieq(name, "cat") || ieq(name, "type")) {
        cmd_cat(name, argc, argv);
    } else if (ieq(name, "mkdir")) {
        cmd_mkdir(argc, argv);
    } else if (ieq(name, "touch")) {
        cmd_touch(argc, argv);
    } else if (ieq(name, "rm") || ieq(name, "rmdir") || ieq(name, "del")) {
        cmd_rm(name, argc, argv);
    } else if (ieq(name, "write")) {
        cmd_write(name, argc, argv, false);
    } else if (ieq(name, "append")) {
        cmd_write(name, argc, argv, true);
    } else if (ieq(name, "cp") || ieq(name, "copy")) {
        cmd_cp(name, argc, argv, false);
    } else if (ieq(name, "mv") || ieq(name, "move")) {
        cmd_cp(name, argc, argv, true);
    } else if (ieq(name, "mount")) {
        cmd_mount();
    } else if (ieq(name, "df")) {
        cmd_df();
    } else if (ieq(name, "echo")) {
        return cmd_echo_redirect(argc, argv);
    } else {
        return false;
    }

    return true;
}
