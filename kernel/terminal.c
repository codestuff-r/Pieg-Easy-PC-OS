
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "panic.h"
#include "fsinit.h"
#include "shellfs.h"

/* ================================================================
 * PIEG SHELL
 * Freestanding C, no dynamic allocation required.
 * ================================================================ */

#define TERMINAL_INPUT_SIZE    256
#define TERMINAL_HISTORY_SIZE  16
#define TERMINAL_ARG_COUNT     16
#define TERMINAL_NAME          "PIEG Shell"
#define TERMINAL_VERSION       "0.5"

typedef struct {
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    bool initialized;
    bool usage_available;
} ramspec_t;

/* Existing hardware and kernel interfaces. */
extern void vga_putchar(char c);
extern void vga_backspace(void);
extern void vga_clear(void);
extern char keyboard_getchar(void);
extern bool get_ramspec(ramspec_t *spec);

/* Shell state. */
static char input[TERMINAL_INPUT_SIZE];
static char command_buffer[TERMINAL_INPUT_SIZE];
static char history[TERMINAL_HISTORY_SIZE][TERMINAL_INPUT_SIZE];

static unsigned int history_count;
static unsigned int history_next;
static int history_position = -1;

/* ================================================================
 * OUTPUT
 * ================================================================ */

static void print(const char *s)
{
    if (!s)
        return;

    while (*s)
        vga_putchar(*s++);
}

static void println(const char *s)
{
    print(s);
    vga_putchar('\n');
}

static void print_u64(uint64_t n)
{
    char buf[20];
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

static void print_hex(uint64_t n)
{
    static const char digits[] = "0123456789ABCDEF";
    bool started = false;

    print("0x");

    for (int shift = 60; shift >= 0; shift -= 4) {
        unsigned int d = (unsigned int)((n >> shift) & 15);

        if (d || started || shift == 0) {
            vga_putchar(digits[d]);
            started = true;
        }
    }
}

static void print_size(uint64_t bytes)
{
    if (bytes >= 1024ull * 1024ull * 1024ull) {
        print_u64(bytes / (1024ull * 1024ull * 1024ull));
        print(" GiB");
    } else if (bytes >= 1024ull * 1024ull) {
        print_u64(bytes / (1024ull * 1024ull));
        print(" MiB");
    } else if (bytes >= 1024ull) {
        print_u64(bytes / 1024ull);
        print(" KiB");
    } else {
        print_u64(bytes);
        print(" bytes");
    }
}

static void print_bool(bool value)
{
    print(value ? "yes" : "no");
}

static void print_repeat(char c, unsigned int n)
{
    while (n--)
        vga_putchar(c);
}

/* ================================================================
 * STRING UTILITIES
 * ================================================================ */

static size_t str_len(const char *s)
{
    size_t n = 0;

    if (s)
        while (s[n])
            ++n;

    return n;
}

static bool str_eq(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (*a++ != *b++)
            return false;
    }

    return *a == *b;
}

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c - 'A' + 'a');

    return c;
}

static bool str_ieq(const char *a, const char *b)
{
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (lower_ascii(*a++) != lower_ascii(*b++))
            return false;
    }

    return *a == *b;
}

static void str_copy(char *dst, const char *src, size_t capacity)
{
    if (!capacity)
        return;

    size_t i = 0;

    if (src) {
        while (src[i] && i + 1 < capacity) {
            dst[i] = src[i];
            ++i;
        }
    }

    dst[i] = '\0';
}

static void trim(char *s)
{
    size_t n = str_len(s);

    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t'))
        s[--n] = '\0';
}

static char *skip_spaces(char *s)
{
    while (*s == ' ' || *s == '\t')
        ++s;

    return s;
}

/* ================================================================
 * PROMPT AND INPUT
 * ================================================================ */

static void prompt(void)
{
    print("pieg:");
    print(shellfs_cwd());
    print(" $ ");
}

static void redraw_line(const char *s)
{
    /*
     * Backspace to the beginning of the current input.
     * This does not clear arbitrary previous screen rows.
     */
    size_t n = str_len(s);

    while (n--)
        vga_backspace();

    print(s);
}

static void history_load(int position, char *dst, size_t capacity)
{
    if (position < 0 || (unsigned int)position >= history_count) {
        if (capacity)
            dst[0] = '\0';
        return;
    }

    unsigned int oldest =
        (history_next + TERMINAL_HISTORY_SIZE - history_count)
        % TERMINAL_HISTORY_SIZE;

    unsigned int index =
        (oldest + (unsigned int)position) % TERMINAL_HISTORY_SIZE;

    str_copy(dst, history[index], capacity);
}

static void read_line(char *dst, size_t capacity)
{
    size_t length = 0;
    char saved[TERMINAL_INPUT_SIZE];

    if (!capacity)
        return;

    dst[0] = '\0';
    history_position = -1;

    for (;;) {
        char c = keyboard_getchar();

        if (!c)
            continue;

        if (c == '\r' || c == '\n') {
            dst[length] = '\0';
            vga_putchar('\n');
            return;
        }

        if (c == '\b' || (unsigned char)c == 127) {
            if (length) {
                dst[--length] = '\0';
                vga_backspace();
            }
            continue;
        }

        /* Ctrl+C cancels the current input. */
        if (c == 3) {
            print("^C\n");
            dst[0] = '\0';
            return;
        }

        /* Ctrl+U clears the input line. */
        if (c == 21) {
            while (length) {
                --length;
                vga_backspace();
            }
            dst[0] = '\0';
            continue;
        }

        /*
         * Ctrl+P recalls older history.
         * Ctrl+N recalls newer history.
         * Arrow keys depend on the keyboard driver's encoding.
         */
        if (c == 16 || c == 14) {
            if (!history_count)
                continue;

            if (c == 16) {
                if (history_position < 0)
                    history_position = (int)history_count - 1;
                else if (history_position > 0)
                    --history_position;
            } else {
                if (history_position >= 0 &&
                    history_position < (int)history_count - 1)
                    ++history_position;
                else
                    history_position = -1;
            }

            if (history_position >= 0)
                history_load(history_position, saved, sizeof(saved));
            else
                saved[0] = '\0';

            while (length) {
                --length;
                vga_backspace();
            }

            str_copy(dst, saved, capacity);
            length = str_len(dst);
            print(dst);
            continue;
        }

        if ((unsigned char)c < 32 || (unsigned char)c > 126)
            continue;

        if (length + 1 < capacity) {
            dst[length++] = c;
            dst[length] = '\0';
            vga_putchar(c);
        }
    }
}

/* ================================================================
 * HISTORY
 * ================================================================ */

static void history_save(const char *s)
{
    if (!s || !*s)
        return;

    if (history_count) {
        unsigned int previous =
            (history_next + TERMINAL_HISTORY_SIZE - 1)
            % TERMINAL_HISTORY_SIZE;

        if (str_eq(history[previous], s))
            return;
    }

    str_copy(history[history_next], s, TERMINAL_INPUT_SIZE);

    history_next = (history_next + 1) % TERMINAL_HISTORY_SIZE;

    if (history_count < TERMINAL_HISTORY_SIZE)
        ++history_count;
}

static void history_show(void)
{
    unsigned int oldest =
        (history_next + TERMINAL_HISTORY_SIZE - history_count)
        % TERMINAL_HISTORY_SIZE;

    if (!history_count) {
        println("Command history is empty.");
        return;
    }

    for (unsigned int i = 0; i < history_count; ++i) {
        unsigned int index = (oldest + i) % TERMINAL_HISTORY_SIZE;

        print("  ");
        print_u64(i + 1);
        print("  ");
        println(history[index]);
    }
}

static void history_clear(void)
{
    history_count = 0;
    history_next = 0;
    history_position = -1;
    println("History cleared.");
}

/* ================================================================
 * ARGUMENT PARSER
 *
 * Supports quoted strings and backslash escaping.
 * Reads from a separate buffer to avoid corrupting the input.
 * ================================================================ */

typedef struct {
    int argc;
    char *argv[TERMINAL_ARG_COUNT];
} command_t;

static command_t parse_command(char *line)
{
    command_t result = {0};
    char *p = line;

    while (*p) {
        p = skip_spaces(p);

        if (!*p || result.argc == TERMINAL_ARG_COUNT)
            break;

        char *start = p;
        char quote = '\0';

        if (*p == '"' || *p == '\'')
            quote = *p++;

        char *write = start;

        if (quote)
            start = p;

        result.argv[result.argc++] = write;

        while (*p) {
            if (quote) {
                if (*p == quote) {
                    ++p;
                    break;
                }
            } else if (*p == ' ' || *p == '\t') {
                break;
            }

            if (*p == '\\' && p[1])
                ++p;

            *write++ = *p++;
        }

        if (!quote) {
            while (*p == ' ' || *p == '\t')
                ++p;
        }

        *write = '\0';

        /*
         * If a quoted token ended, p already points after its quote.
         * Advance past following whitespace before parsing the next one.
         */
        p = skip_spaces(p);
    }

    return result;
}

/* ================================================================
 * RAM AND SYSTEM INFORMATION
 * ================================================================ */

static void show_ram(void)
{
    ramspec_t ram;

    if (!get_ramspec(&ram) || !ram.initialized) {
        println("RAM information unavailable.");
        return;
    }

    print("Usable RAM: ");
    print_size(ram.total_bytes);
    vga_putchar('\n');

    if (ram.usage_available) {
        print("Used RAM:   ");
        print_size(ram.used_bytes);
        vga_putchar('\n');

        print("Free RAM:   ");
        print_size(ram.free_bytes);
        vga_putchar('\n');
    } else {
        println("Used RAM:   unavailable, no allocator accounting");
        println("Free RAM:   unavailable, no allocator accounting");
    }
}

static void show_about(void)
{
    println("PIEG OS");
    println("Pieg Isn't Even GNU");
    println("Custom x86-64 operating system");
    println("Boot protocol: Multiboot2");
    println("Kernel language: C");
}

static void show_version(void)
{
    print("PIEG Shell ");
    println(TERMINAL_VERSION);
    println("PIEG OS x86-64");
}

static void show_sysinfo(void)
{
    println("System information");
    print_repeat('=', 40);
    vga_putchar('\n');

    println("OS:           PIEG OS");
    println("Architecture: x86-64");
    println("Boot:         Multiboot2");
    print("Shell:        ");
    println(TERMINAL_NAME);
    print("Version:      ");
    println(TERMINAL_VERSION);

    print("Filesystem mounted: ");
    print_bool(fs_is_mounted());
    vga_putchar('\n');

    if (fs_is_mounted()) {
        print("Filesystem: ");
        println(fs_type_name());

        print("Disk sectors: ");
        print_u64(fs_disk_sectors());
        vga_putchar('\n');
    }

    vga_putchar('\n');
    show_ram();
}

static void show_fastfetch(void)
{
    println("      ______ _____ _____");
    println("     |  __  |  ___|  __ \\");
    println("     | |__) | |__ | |__) |");
    println("     |  ___/|  __||  ___/");
    println("     | |    | |___| |");
    println("     |_|    |_____|_|");
    vga_putchar('\n');

    println("PIEG OS");
    println("Pieg Isn't Even GNU");
    println("x86_64 / Multiboot2");

    print("RAM: ");
    ramspec_t ram;

    if (get_ramspec(&ram) && ram.initialized)
        print_size(ram.total_bytes);
    else
        print("unavailable");

    vga_putchar('\n');

    print("Filesystem: ");

    if (fs_is_mounted()) {
        print(fs_type_name());
        print(" mounted");
    } else {
        print("not mounted");
    }

    vga_putchar('\n');
}

/* ================================================================
 * HELP
 * ================================================================ */

static void show_help(const char *category)
{
    if (category && str_ieq(category, "system")) {
        println("about, ver, version, uname, hostname");
        println("sysinfo, fastfetch, ram, free, hex");
        println("clear, cls, reboot, halt, shutdown");
        return;
    }

    if (category &&
        (str_ieq(category, "files") || str_ieq(category, "filesystem"))) {
        println("ls, dir, cd, pwd, cat, type, mount, df");
        println("mkdir, touch, rm, rmdir, del, cp, mv, write, append");
        println("echo text > file, echo text >> file");
        println("Layout: /# system, /PROGRAMS, /U/HOME (~ = home).");
        return;
    }

    if (category && str_ieq(category, "shell")) {
        println("help, echo, history, history-clear, whoami");
        println("true, false, exit");
        println("Quoted arguments: echo \"hello PIEG\"");
        println("Ctrl+C cancels input, Ctrl+U clears input.");
        println("Ctrl+P/Ctrl+N navigate command history.");
        return;
    }

    println("PIEG Shell commands");
    print_repeat('-', 40);
    vga_putchar('\n');

    println("General:");
    println("  help [category]   Show help");
    println("  clear, cls        Clear screen");
    println("  echo [text]       Print arguments");
    println("  history           Show command history");
    println("  history-clear     Clear command history");
    println("  whoami            Show current account");
    println("  pwd               Show current path");
    println("  exit              Explain shell exit behavior");
    println("  true, false       Basic shell commands");

    vga_putchar('\n');
    println("System:");
    println("  about             About PIEG");
    println("  ver, version      Show version");
    println("  uname             Show OS identity");
    println("  hostname          Show hostname");
    println("  sysinfo           Full system summary");
    println("  fastfetch         Compact system summary");
    println("  ram, free         RAM information");

    vga_putchar('\n');
    println("Storage:");
    println("  ls, dir [path]    List a directory");
    println("  cd [path]         Change directory (no argument: home)");
    println("  cat, type <file>  Print a file");
    println("  mkdir <dir>       Create a directory");
    println("  touch <file>      Create an empty file");
    println("  rm, rmdir <path>  Remove a file or empty directory");
    println("  cp, mv <a> <b>    Copy or move a file");
    println("  write <f> <text>  Overwrite a file with text");
    println("  append <f> <text> Append text to a file");
    println("  echo t > file     Redirect (>> appends)");
    println("  mount             Devices and filesystem status");
    println("  df                Volume usage");

    vga_putchar('\n');
    println("Diagnostics and power:");
    println("  hex <decimal>     Convert number to hexadecimal");
    println("  panic             Trigger a kernel panic");
    println("  reboot            Reboot machine");
    println("  halt, shutdown    Halt CPU");

    if (category) {
        vga_putchar('\n');
        print("Unknown help category: ");
        println(category);
        println("Categories: system, files, shell");
    }
}

/* ================================================================
 * NUMERIC UTILITIES
 * ================================================================ */

static bool parse_decimal(const char *s, uint64_t *out)
{
    if (!s || !*s || !out)
        return false;

    uint64_t value = 0;

    while (*s) {
        if (*s < '0' || *s > '9')
            return false;

        uint64_t digit = (uint64_t)(*s - '0');

        if (value > (UINT64_MAX - digit) / 10)
            return false;

        value = value * 10 + digit;
        ++s;
    }

    *out = value;
    return true;
}

/* ================================================================
 * HARDWARE CONTROL
 * ================================================================ */

static void reboot_system(void)
{
    println("Rebooting PIEG...");
    __asm__ volatile ("cli");

    for (unsigned int i = 0; i < 100000; ++i) {
        uint8_t status;

        __asm__ volatile ("inb $0x64, %0" : "=a"(status));

        if (!(status & 2))
            break;
    }

    __asm__ volatile (
        "movb $0xFE, %%al\n"
        "outb %%al, $0x64\n"
        :
        :
        : "al"
    );

    for (;;)
        __asm__ volatile ("hlt");
}

static void halt_system(void)
{
    println("PIEG halted.");
    __asm__ volatile ("cli");

    for (;;)
        __asm__ volatile ("hlt");
}

/* ================================================================
 * COMMAND DISPATCH
 * ================================================================ */

static void execute_command(const char *line)
{
    str_copy(command_buffer, line, sizeof(command_buffer));
    trim(command_buffer);

    char *trimmed = skip_spaces(command_buffer);

    if (!*trimmed)
        return;

    history_save(trimmed);

    command_t cmd = parse_command(trimmed);

    if (!cmd.argc)
        return;

    const char *name = cmd.argv[0];
    const char *arg = cmd.argc > 1 ? cmd.argv[1] : "";

    if (shellfs_command(cmd.argc, cmd.argv))
        return;

    if (str_ieq(name, "help") || str_eq(name, "?")) {
        show_help(cmd.argc > 1 ? arg : NULL);
    } else if (str_ieq(name, "echo")) {
        for (int i = 1; i < cmd.argc; ++i) {
            if (i > 1)
                vga_putchar(' ');
            print(cmd.argv[i]);
        }
        vga_putchar('\n');
    } else if (str_ieq(name, "clear") || str_ieq(name, "cls")) {
        vga_clear();
    } else if (str_ieq(name, "history")) {
        history_show();
    } else if (str_ieq(name, "history-clear")) {
        history_clear();
    } else if (str_ieq(name, "whoami")) {
        println("root");
        println("PIEG user accounts are not implemented yet.");
    } else if (str_ieq(name, "exit")) {
        println("The PIEG shell runs inside the kernel.");
        println("There is no parent process to return to.");
    } else if (str_ieq(name, "true")) {
        /* Intentionally no output. */
    } else if (str_ieq(name, "false")) {
        println("false");
    } else if (str_ieq(name, "about")) {
        show_about();
    } else if (str_ieq(name, "ver") || str_ieq(name, "version")) {
        show_version();
    } else if (str_ieq(name, "uname")) {
        println("PIEG pieg 0.3 x86_64 Multiboot2");
    } else if (str_ieq(name, "hostname")) {
        println("pieg");
    } else if (str_ieq(name, "sysinfo") || str_ieq(name, "systeminfo")) {
        show_sysinfo();
    } else if (str_ieq(name, "fastfetch")) {
        show_fastfetch();
    } else if (str_ieq(name, "ram") || str_ieq(name, "free")) {
        show_ram();
    } else if (str_ieq(name, "hex")) {
        uint64_t value;

        if (cmd.argc != 2 || !parse_decimal(arg, &value)) {
            println("Usage: hex <decimal-number>");
        } else {
            print(arg);
            print(" = ");
            print_hex(value);
            vga_putchar('\n');
        }
    } else if (str_ieq(name, "panic")) {
        /*
         * panic.h requires both arguments.
         * The kernel_panic function is noreturn.
         */
        kernel_panic(
            cmd.argc > 1 ? arg : "Manual panic requested",
            cmd.argc > 2 ? cmd.argv[2] : "Triggered by PIEG Shell"
        );
    } else if (str_ieq(name, "reboot")) {
        reboot_system();
    } else if (str_ieq(name, "halt") || str_ieq(name, "shutdown")) {
        halt_system();
    } else if (str_ieq(name, "sudo")) {
        println("PIEG does not implement privilege separation.");
        println("The shell currently executes in kernel context.");
    } else if (str_ieq(name, "ps") || str_ieq(name, "top") ||
               str_ieq(name, "kill")) {
        println("Process management is not exposed by the shell.");
    } else if (str_ieq(name, "network") || str_ieq(name, "ifconfig") ||
               str_ieq(name, "ping")) {
        println("No network command interface is registered.");
    } else if (str_ieq(name, "date") || str_ieq(name, "time")) {
        println("System clock support is not exposed by the shell.");
    } else {
        print("Unknown command: ");
        println(name);
        println("Type 'help' for available commands.");
    }
}

/* ================================================================
 * ENTRY POINT
 * ================================================================ */

void terminal_run(void)
{
    vga_clear();

    println("PIEG Shell");
    println("Pieg Isn't Even GNU");
    print("Version ");
    println(TERMINAL_VERSION);
    println("Type 'help' for commands.");
    shellfs_start();
    vga_putchar('\n');

    for (;;) {
        prompt();
        read_line(input, sizeof(input));
        execute_command(input);
    }
}
