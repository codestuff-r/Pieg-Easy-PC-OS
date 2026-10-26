#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "fsinit.h"
#include "program.h"
#include "shellfs.h"

/* ================================================================
 * PIEG SHELL
 * Freestanding C, no dynamic allocation required.
 *
 * Only shell-state commands are built in (cd, help, history, exit).
 * Everything else is a program in /PROGRAMS, run by name.
 * ================================================================ */

#define TERMINAL_INPUT_SIZE    256
#define TERMINAL_HISTORY_SIZE  16
#define TERMINAL_ARG_COUNT     16
#define TERMINAL_NAME          "PIEG Shell"
#define TERMINAL_VERSION       PIEG_SHELL_VERSION

/* Existing hardware and kernel interfaces. */
extern void vga_putchar(char c);
extern void vga_backspace(void);
extern void vga_clear(void);
extern void vga_scroll_view(int direction);
extern char keyboard_getchar(void);

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

        /* Ctrl+Up / Ctrl+Down scroll the terminal's retained output. */
        if ((unsigned char)c == 0x80) {
            vga_scroll_view(-3);
            continue;
        }
        if ((unsigned char)c == 0x81) {
            vga_scroll_view(3);
            continue;
        }

        /* Any normal input returns the view to the live prompt. */
        vga_scroll_view(0);

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
 * HELP
 * ================================================================ */

typedef struct {
    unsigned int column;
    unsigned int count;
} help_context_t;

/* Prints every *.ELF in /PROGRAMS as a command name, wrapped to the screen. */
static void help_program(const char *name, uint8_t attributes,
                         uint32_t size, void *context)
{
    help_context_t *h = (help_context_t *)context;
    size_t length = str_len(name);
    char shown[32];

    (void)size;

    if ((attributes & FAT_ATTR_DIRECTORY) || length < 5 ||
        !str_ieq(name + length - 4, ".elf"))
        return;

    length -= 4;

    if (length >= sizeof(shown))
        length = sizeof(shown) - 1;

    for (size_t i = 0; i < length; i++)
        shown[i] = lower_ascii(name[i]);

    shown[length] = '\0';

    if (h->column && h->column + 1 + length > 76) {
        vga_putchar('\n');
        h->column = 0;
    }

    if (h->column == 0) {
        print("  ");
        h->column = 2;
    } else {
        vga_putchar(' ');
        h->column++;
    }

    print(shown);
    h->column += (unsigned int)length;
    h->count++;
}

static void show_help(void)
{
    println(TERMINAL_NAME " " TERMINAL_VERSION);
    println("Built-in: cd, help, history, history-clear, exit");
    println("Programs in /PROGRAMS (run by name):");

    help_context_t h = {0, 0};

    if (!fs_is_mounted() ||
        fs_list(PIEG_DIR_PROGRAMS, help_program, &h) != FAT_OK ||
        h.count == 0) {
        println("  (none)");
    } else if (h.column) {
        vga_putchar('\n');
    }

    println("Add your own .ELF programs to /PROGRAMS (see programs/hello.c).");
    println("Ctrl+Up/Down scroll, Ctrl+C cancel, Ctrl+U clear line,");
    println("Ctrl+P/Ctrl+N history.");
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

    if (str_ieq(name, "cd")) {
        shellfs_cd(cmd.argc > 1 ? cmd.argv[1] : "~");
    } else if (str_ieq(name, "help") || str_eq(name, "?")) {
        show_help();
    } else if (str_ieq(name, "history")) {
        history_show();
    } else if (str_ieq(name, "history-clear")) {
        history_clear();
    } else if (str_ieq(name, "exit")) {
        println("The PIEG shell runs inside the kernel.");
        println("There is no parent process to return to.");
    } else {
        int status;

        if (!program_run(cmd.argc, cmd.argv, &status)) {
            print("Unknown command: ");
            println(name);
            println("Type 'help' for built-ins and programs.");
        }
    }
}

/* ================================================================
 * ENTRY POINT
 * ================================================================ */

void terminal_run(void)
{
    vga_clear();

    println(TERMINAL_NAME);
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
