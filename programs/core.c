#include "libpieg.h"

/*
 * Core commands as one multi-call program. /PROGRAMS holds a copy of this
 * binary under each command name (LS.ELF, CAT.ELF, ...); the command is
 * picked from argv[0].
 */

#define PATH_MAX 256

static char io_buffer[4096];

/* ================================================================
 * Helpers
 * ================================================================ */

static void fail(const char *command, const char *path, int error)
{
    p_puts(command);
    p_puts(": ");

    if (path) {
        p_puts(path);
        p_puts(": ");
    }

    p_putln(pieg->fs_error(error));
}

static void resolve(const char *arg, char *out)
{
    pieg->resolve(arg, out, PATH_MAX);
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
 * Files
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
    int directory = (attributes & PIEG_ATTR_DIRECTORY) != 0;

    p_puts(directory ? "[DIR]  " : "[FILE] ");
    p_puts(name);

    if (directory) {
        stats->directories++;
    } else {
        p_puts("  ");
        p_u64(size);
        p_puts(" bytes");
        stats->files++;
        stats->bytes += size;
    }

    p_putc('\n');
}

static int cmd_ls(const char *name, int argc, char **argv)
{
    const char *target = ".";
    char path[PATH_MAX];
    int directory;
    uint32_t size;

    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            target = argv[i];
            break;
        }
    }

    resolve(target, path);

    int r = pieg->fs_stat(path, &directory, &size);

    if (r) {
        fail(name, path, r);
        return 1;
    }

    if (!directory) {
        p_puts("[FILE] ");
        p_puts(path);
        p_puts("  ");
        p_u64(size);
        p_putln(" bytes");
        return 0;
    }

    list_stats_t stats = {0, 0, 0};

    p_puts(path);
    p_putln(":");

    r = pieg->fs_list(path, list_entry, &stats);

    if (r) {
        fail(name, path, r);
        return 1;
    }

    p_puts("Files: ");
    p_u64(stats.files);
    p_puts("  Directories: ");
    p_u64(stats.directories);
    p_puts("  Bytes: ");
    p_u64(stats.bytes);
    p_putc('\n');
    return 0;
}

static int cmd_cat(const char *name, int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        p_puts("Usage: ");
        p_puts(name);
        p_putln(" <file>");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX];
        int directory;
        uint32_t size;

        resolve(argv[i], path);

        int r = pieg->fs_stat(path, &directory, &size);

        if (!r && directory)
            r = 6;                  /* FAT_ERR_ISDIR */

        if (r) {
            fail(name, path, r);
            status = 1;
            continue;
        }

        uint32_t offset = 0;
        char last = '\n';

        while (offset < size) {
            uint32_t got = 0;

            r = pieg->fs_read(path, offset, io_buffer, 512, &got);

            if (r || got == 0) {
                if (r) {
                    fail(name, path, r);
                    status = 1;
                }
                break;
            }

            for (uint32_t k = 0; k < got; k++) {
                char c = io_buffer[k];

                if (c == '\r')
                    continue;

                if (c != '\n' && c != '\t' && (c < 32 || c > 126))
                    c = '.';

                p_putc(c);
                last = c;
            }

            offset += got;
        }

        if (last != '\n')
            p_putc('\n');
    }

    return status;
}

static int cmd_mkdir(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        p_putln("Usage: mkdir <directory>...");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX];

        resolve(argv[i], path);

        int r = pieg->fs_mkdir(path);

        if (r) {
            fail("mkdir", path, r);
            status = 1;
        }
    }

    return status;
}

static int cmd_touch(int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        p_putln("Usage: touch <file>...");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX];

        resolve(argv[i], path);

        if (pieg->fs_stat(path, 0, 0) == 0)
            continue;

        int r = pieg->fs_create(path);

        if (r) {
            fail("touch", path, r);
            status = 1;
        }
    }

    return status;
}

static int cmd_rm(const char *name, int argc, char **argv)
{
    int status = 0;

    if (argc < 2) {
        p_puts("Usage: ");
        p_puts(name);
        p_putln(" <path>...");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        char path[PATH_MAX];

        resolve(argv[i], path);

        int r = pieg->fs_remove(path);

        if (r) {
            fail(name, path, r);
            status = 1;
        }
    }

    return status;
}

static int cmd_write(const char *name, int argc, char **argv, int append)
{
    char path[PATH_MAX];
    char text[512];

    if (argc < 3) {
        p_puts("Usage: ");
        p_puts(name);
        p_putln(" <file> <text...>");
        return 1;
    }

    resolve(argv[1], path);

    uint32_t n = join_args(argc, argv, 2, text, sizeof(text));
    int r = pieg->fs_write(path, text, n, append);

    if (r) {
        fail(name, path, r);
        return 1;
    }

    return 0;
}

/* echo text > file / echo text >> file; plain echo prints its arguments. */
static int cmd_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        int append = p_streq(argv[i], ">>");

        if (!append && !p_streq(argv[i], ">"))
            continue;

        if (i + 1 >= argc) {
            p_putln("echo: missing file name after redirection");
            return 1;
        }

        char path[PATH_MAX];
        char text[512];

        resolve(argv[i + 1], path);

        uint32_t n = join_args(i, argv, 1, text, sizeof(text));
        int r = pieg->fs_write(path, text, n, append);

        if (r) {
            fail("echo", path, r);
            return 1;
        }

        return 0;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1)
            p_putc(' ');
        p_puts(argv[i]);
    }

    p_putc('\n');
    return 0;
}

/* Copies one file; dst may be a directory. */
static int copy_file(const char *src, const char *dst_arg, char *final_dst)
{
    char dst[PATH_MAX];
    int directory;
    uint32_t size;

    int r = pieg->fs_stat(src, &directory, &size);

    if (r)
        return r;

    if (directory)
        return 6;                   /* FAT_ERR_ISDIR */

    resolve(dst_arg, dst);

    if (pieg->fs_stat(dst, &directory, 0) == 0 && directory) {
        size_t n = p_strlen(dst);
        const char *base = base_name(src);

        if (n + 1 + p_strlen(base) + 1 > sizeof(dst))
            return 8;               /* FAT_ERR_NAME */

        if (n > 1)
            dst[n++] = '/';

        for (const char *p = base; *p; p++)
            dst[n++] = *p;

        dst[n] = '\0';
    }

    for (size_t i = 0; i < PATH_MAX; i++) {
        final_dst[i] = dst[i];

        if (!dst[i])
            break;
    }

    /* Creates or truncates the target, then streams the data in. */
    r = pieg->fs_write(dst, "", 0, 0);

    if (r)
        return r;

    uint32_t offset = 0;

    while (offset < size) {
        uint32_t got = 0;

        r = pieg->fs_read(src, offset, io_buffer, sizeof(io_buffer), &got);

        if (r)
            return r;

        if (got == 0)
            break;

        r = pieg->fs_write(dst, io_buffer, got, 1);

        if (r)
            return r;

        offset += got;
    }

    return 0;
}

static int cmd_cp(const char *name, int argc, char **argv, int move)
{
    char src[PATH_MAX];
    char dst[PATH_MAX];

    if (argc != 3) {
        p_puts("Usage: ");
        p_puts(name);
        p_putln(" <source> <destination>");
        return 1;
    }

    resolve(argv[1], src);

    int r = copy_file(src, argv[2], dst);

    if (r) {
        fail(name, src, r);
        return 1;
    }

    if (move) {
        r = pieg->fs_remove(src);

        if (r) {
            fail(name, src, r);
            return 1;
        }
    }

    return 0;
}

static int cmd_pwd(void)
{
    p_putln(pieg->cwd());
    return 0;
}

/* ================================================================
 * Storage status
 * ================================================================ */

static int cmd_mount(void)
{
    pieg_fsinfo_t fs;
    int mounted = pieg->fs_info(&fs);

    p_putln("Block devices");

    for (unsigned int i = 0; i < pieg->blockdev_count(); i++) {
        pieg_bdinfo_t dev;

        if (!pieg->blockdev_info(i, &dev))
            continue;

        p_puts("  ");
        p_puts(dev.name);
        p_puts("  ");
        p_size(dev.bytes);
        p_puts(dev.writable ? "  rw" : "  ro");

        if (dev.mounted)
            p_puts("  mounted on /");

        p_putc('\n');
    }

    if (!mounted) {
        p_putln("No filesystem mounted.");
        return 0;
    }

    p_puts("Filesystem: ");
    p_puts(fs.type);
    p_puts("  label=");
    p_putln(fs.label[0] ? fs.label : "(none)");

    p_puts("PIEG volume: ");
    p_putln(fs.pieg_volume ? "yes" : "no");

    p_puts("Access: ");
    p_putln(fs.writable ? "read/write" : "read-only");
    return 0;
}

static int cmd_df(void)
{
    pieg_fsinfo_t fs;
    uint64_t total;
    uint64_t free_bytes;

    int r = pieg->fs_df(&total, &free_bytes);

    if (r) {
        fail("df", 0, r);
        return 1;
    }

    pieg->fs_info(&fs);

    p_puts("Filesystem: ");
    p_puts(fs.type);
    p_puts(" on ");
    p_putln(fs.device);

    p_puts("Total: ");
    p_size(total);
    p_putc('\n');

    p_puts("Used:  ");
    p_size(total - free_bytes);
    p_putc('\n');

    p_puts("Free:  ");
    p_size(free_bytes);
    p_putc('\n');
    return 0;
}

/* ================================================================
 * System information
 * ================================================================ */

static int cmd_ram(void)
{
    pieg_raminfo_t ram;

    if (!pieg->ram_info(&ram)) {
        p_putln("RAM information unavailable.");
        return 1;
    }

    p_puts("Usable RAM: ");
    p_size(ram.total);
    p_putc('\n');

    if (ram.usage_available) {
        p_puts("Used RAM:   ");
        p_size(ram.used);
        p_putc('\n');

        p_puts("Free RAM:   ");
        p_size(ram.free);
        p_putc('\n');
    } else {
        p_putln("Used RAM:   unavailable, no allocator accounting");
        p_putln("Free RAM:   unavailable, no allocator accounting");
    }

    return 0;
}

static int cmd_about(void)
{
    p_putln("PIEG OS");
    p_putln("Pieg Isn't Even GNU");
    p_putln("Custom x86-64 operating system");
    p_putln("Boot protocol: Multiboot2");
    p_putln("Kernel language: C");
    return 0;
}

static int cmd_ver(void)
{
    p_puts("PIEG Shell ");
    p_putln(pieg->shell_version);
    p_putln("PIEG OS x86-64");
    return 0;
}

static int cmd_sysinfo(void)
{
    pieg_fsinfo_t fs;
    int mounted = pieg->fs_info(&fs);

    p_putln("System information");

    for (int i = 0; i < 40; i++)
        p_putc('=');

    p_putc('\n');

    p_putln("OS:           PIEG OS");
    p_putln("Architecture: x86-64");
    p_putln("Boot:         Multiboot2");
    p_putln("Shell:        PIEG Shell");
    p_puts("Version:      ");
    p_putln(pieg->shell_version);

    p_puts("Filesystem mounted: ");
    p_putln(mounted ? "yes" : "no");

    if (mounted) {
        p_puts("Filesystem: ");
        p_putln(fs.type);

        p_puts("Disk sectors: ");
        p_u64(fs.sectors);
        p_putc('\n');
    }

    p_putc('\n');
    return cmd_ram();
}

static int cmd_fetch(void)
{
    pieg_fsinfo_t fs;
    pieg_raminfo_t ram;

    p_putln("      ______ _____ _____");
    p_putln("     |  __  |  ___|  __ \\");
    p_putln("     | |__) | |__ | |__) |");
    p_putln("     |  ___/|  __||  ___/");
    p_putln("     | |    | |___| |");
    p_putln("     |_|    |_____|_|");
    p_putc('\n');

    p_putln("PIEG OS");
    p_putln("Pieg Isn't Even GNU");
    p_putln("x86_64 / Multiboot2");

    p_puts("RAM: ");

    if (pieg->ram_info(&ram))
        p_size(ram.total);
    else
        p_puts("unavailable");

    p_putc('\n');

    p_puts("Filesystem: ");

    if (pieg->fs_info(&fs)) {
        p_puts(fs.type);
        p_puts(" mounted");
    } else {
        p_puts("not mounted");
    }

    p_putc('\n');
    return 0;
}

static int parse_decimal(const char *s, uint64_t *out)
{
    uint64_t value = 0;

    if (!*s)
        return 0;

    while (*s) {
        if (*s < '0' || *s > '9')
            return 0;

        uint64_t digit = (uint64_t)(*s - '0');

        if (value > (UINT64_MAX - digit) / 10)
            return 0;

        value = value * 10 + digit;
        s++;
    }

    *out = value;
    return 1;
}

static int cmd_hex(int argc, char **argv)
{
    uint64_t value;

    if (argc != 2 || !parse_decimal(argv[1], &value)) {
        p_putln("Usage: hex <decimal-number>");
        return 1;
    }

    p_puts(argv[1]);
    p_puts(" = ");
    p_hex(value);
    p_putc('\n');
    return 0;
}

static int cmd_whoami(void)
{
    p_putln("root");
    p_putln("PIEG user accounts are not implemented yet.");
    return 0;
}

/* ================================================================
 * Dispatch
 * ================================================================ */

/* Command name from argv[0]: no directory, no ".ELF", lower case. */
static void command_name(const char *arg, char *out, size_t capacity)
{
    const char *base = arg;

    for (const char *p = arg; *p; p++) {
        if (*p == '/' && p[1])
            base = p + 1;
    }

    size_t n = p_strlen(base);

    if (n > 4 && p_ieq(base + n - 4, ".elf"))
        n -= 4;

    if (n >= capacity)
        n = capacity - 1;

    for (size_t i = 0; i < n; i++)
        out[i] = p_lower(base[i]);

    out[n] = '\0';
}

#define IS(a) p_streq(name, a)

int main(int argc, char **argv)
{
    char name[16];

    command_name(argv[0], name, sizeof(name));

    if (IS("ls") || IS("dir"))
        return cmd_ls(name, argc, argv);
    if (IS("cat") || IS("type"))
        return cmd_cat(name, argc, argv);
    if (IS("mkdir"))
        return cmd_mkdir(argc, argv);
    if (IS("touch"))
        return cmd_touch(argc, argv);
    if (IS("rm") || IS("rmdir") || IS("del"))
        return cmd_rm(name, argc, argv);
    if (IS("write"))
        return cmd_write(name, argc, argv, 0);
    if (IS("append"))
        return cmd_write(name, argc, argv, 1);
    if (IS("cp") || IS("copy"))
        return cmd_cp(name, argc, argv, 0);
    if (IS("mv") || IS("move"))
        return cmd_cp(name, argc, argv, 1);
    if (IS("echo"))
        return cmd_echo(argc, argv);
    if (IS("pwd"))
        return cmd_pwd();
    if (IS("mount"))
        return cmd_mount();
    if (IS("df"))
        return cmd_df();
    if (IS("clear") || IS("cls")) {
        pieg->clear();
        return 0;
    }
    if (IS("whoami"))
        return cmd_whoami();
    if (IS("about"))
        return cmd_about();
    if (IS("ver") || IS("version"))
        return cmd_ver();
    if (IS("uname")) {
        p_putln("PIEG pieg 0.7 x86_64 Multiboot2");
        return 0;
    }
    if (IS("hostname")) {
        p_putln("pieg");
        return 0;
    }
    if (IS("sysinfo"))
        return cmd_sysinfo();
    if (IS("fetch"))
        return cmd_fetch();
    if (IS("ram") || IS("free"))
        return cmd_ram();
    if (IS("hex"))
        return cmd_hex(argc, argv);
    if (IS("panic")) {
        pieg->panic(argc > 1 ? argv[1] : "Manual panic requested",
                    argc > 2 ? argv[2] : "Triggered by PIEG Shell");
    }
    if (IS("reboot"))
        pieg->reboot();
    if (IS("halt") || IS("shutdown"))
        pieg->halt();

    p_puts(name);
    p_putln(": core multi-call binary, run it under a command name.");
    return 1;
}
