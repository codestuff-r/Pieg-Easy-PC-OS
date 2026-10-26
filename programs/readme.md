# Making PIEG programs

A program is a C file in `programs/`. `make` builds it into
`/PROGRAMS/NAME.ELF`, and you run it by typing `name` in the PIEG shell.

## Quick start

Create `programs/greet.c`:

```c
#include "libpieg.h"

int main(int argc, char **argv)
{
    p_puts("Hello, ");
    p_putln(argc > 1 ? argv[1] : "world");
    return 0;
}
```

Build and install:

```
make run                # builds everything, refreshes the disk, boots QEMU
make install-programs   # only refresh programs on an existing disk image
```

In PIEG: `greet Anton` prints `Hello, Anton`.

## Rules

- File name: `programs/NAME.c` becomes `/PROGRAMS/NAME.ELF`, run as `name`.
  Keep NAME to 8 characters or fewer (FAT 8.3).
- Do not use `core.c` or `libpieg.c` as names. `core.c` is the built-in
  command set, `libpieg.c` is the runtime.
- Write `int main(int argc, char **argv)`. `argv[0]` is the name as typed.
  Return an exit code (the shell currently ignores it).
- Include only `libpieg.h`. There is no libc: no `printf`, `malloc`,
  `strlen` (use `p_strlen`), or `<string.h>`.
- Programs run in kernel context with no privilege separation. A bad pointer
  or an infinite loop takes down the whole system. Ctrl+C cannot stop a
  running program.
- Memory: the program image (code, data, BSS) must fit in 1 MiB. There is no
  heap; use static buffers. The stack is the kernel stack (64 KiB), so keep
  large buffers `static`, not local.
- A program cannot start another program.

## Runtime helpers (`libpieg.h`)

| Function | Does |
|---|---|
| `p_putc(c)`, `p_puts(s)`, `p_putln(s)` | Print a char, string, string + newline |
| `p_u64(n)`, `p_hex(n)`, `p_size(bytes)` | Print decimal, `0x` hex, or `12 MiB` style size |
| `p_getc()` | Wait for a key and return it |
| `p_strlen`, `p_streq`, `p_ieq`, `p_lower` | Basic string helpers (`p_ieq` ignores case) |
| `memcpy`, `memmove`, `memset`, `memcmp` | Provided for you |

Everything else goes through the global `pieg` pointer (see `kernel/api.h`).

## Kernel API (`pieg->...`)

Console:

- `putchar(c)`, `write(s)`: print.
- `getchar()`: non-blocking, returns 0 when no key is ready. Use `p_getc()`
  to block.
- `clear()`: clear the screen.

Paths:

- `cwd()`: current directory.
- `resolve(arg, out, cap)`: turns user input (`file.txt`, `../x`, `~/a`) into
  an absolute path. Always resolve user paths before using `fs_*`.

Filesystem (return 0 on success, an error code otherwise):

- `fs_stat(path, &is_dir, &size)`
- `fs_list(path, callback, ctx)`: callback gets `(name, attributes, size, ctx)`;
  directories have `PIEG_ATTR_DIRECTORY` set.
- `fs_read(path, offset, buf, len, &done)`
- `fs_write(path, data, len, append)`: creates the file if missing; `append`
  0 overwrites.
- `fs_create(path)`, `fs_mkdir(path)`, `fs_remove(path)` (files and empty
  directories)
- `fs_df(&total, &free)`
- `fs_error(code)`: error code to text, for example
  `p_putln(pieg->fs_error(r));`

New files get 8.3 names only.

System:

- `fs_info(&info)`, `blockdev_count()`, `blockdev_info(i, &info)`,
  `ram_info(&info)`: return 0 when unavailable.
- `shell_version`: the shell version string.
- `panic(reason, details)`, `reboot()`, `halt()`: never return.

## Example: read a file

```c
#include "libpieg.h"

static char buf[512];

int main(int argc, char **argv)
{
    char path[256];
    uint32_t got;

    if (argc < 2) {
        p_putln("Usage: show <file>");
        return 1;
    }

    pieg->resolve(argv[1], path, sizeof(path));

    int r = pieg->fs_read(path, 0, buf, sizeof(buf) - 1, &got);

    if (r) {
        p_putln(pieg->fs_error(r));
        return 1;
    }

    buf[got] = '\0';
    p_puts(buf);
    return 0;
}
```

## Troubleshooting

- `Unknown command`: the program is not on the disk PIEG booted. Run
  `make install-programs`, or `make run`.
- `cannot run ...: program too large`: image over 1 MiB; shrink static
  buffers.
- `unsupported relocation`: use the Makefile build; it links with the flags
  the loader needs. Avoid building programs by hand.
- `help` lists everything currently in `/PROGRAMS`.

## Changing the API

Add the function to `pieg_api_t` in `kernel/api.h`, implement and add it to
the table in `kernel/program.c`, and bump `PIEG_API_VERSION`. Rebuild the
kernel and programs together.
