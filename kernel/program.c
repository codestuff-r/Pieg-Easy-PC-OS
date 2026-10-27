#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "api.h"
#include "blockdev.h"
#include "fat.h"
#include "fsinit.h"
#include "mem.h"
#include "panic.h"
#include "pci.h"
#include "persist.h"
#include "program.h"
#include "ramspec.h"
#include "shellfs.h"

extern void vga_putchar(char c);
extern void vga_write(const char *text);
extern void vga_clear(void);
extern void vga_scroll_view(int direction);
extern char keyboard_getchar(void);

/* ================================================================
 * Program memory
 * ================================================================ */

#define PROGRAM_MEM_SIZE (1024u * 1024u)

static uint8_t program_memory[PROGRAM_MEM_SIZE]
    __attribute__((aligned(4096)));

/* ================================================================
 * API implementation
 * ================================================================ */

static void copy_str(char *dst, const char *src, size_t capacity)
{
    size_t i = 0;

    if (!capacity)
        return;

    while (src && src[i] && i + 1 < capacity) {
        dst[i] = src[i];
        i++;
    }

    dst[i] = '\0';
}

static bool str_same(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++)
            return false;
    }

    return *a == *b;
}

static char api_getchar(void)
{
    char c = keyboard_getchar();

    if ((unsigned char)c == 0x80) {
        vga_scroll_view(-3);
        return 0;
    }

    if ((unsigned char)c == 0x81) {
        vga_scroll_view(3);
        return 0;
    }

    if (c)
        vga_scroll_view(0);

    return c;
}

static void api_resolve(const char *arg, char *out, size_t capacity)
{
    fs_resolve(shellfs_cwd(), arg, out, capacity);
}

static int api_fs_list(const char *path, pieg_list_cb callback,
                       void *context)
{
    return (int)fs_list(path, callback, context);
}

static int api_fs_stat(const char *path, int *is_directory, uint32_t *size)
{
    bool directory = false;
    uint32_t bytes = 0;
    fat_err_t r = fs_stat(path, &directory, &bytes);

    if (is_directory)
        *is_directory = directory;
    if (size)
        *size = bytes;

    return (int)r;
}

static int api_fs_read(const char *path, uint32_t offset, void *buffer,
                       uint32_t length, uint32_t *done)
{
    return (int)fs_read(path, offset, buffer, length, done);
}

static int api_fs_write(const char *path, const void *data, uint32_t length,
                        int append)
{
    return (int)fs_write(path, data, length, append != 0);
}

static int api_fs_create(const char *path)
{
    return (int)fs_create(path);
}

static int api_fs_mkdir(const char *path)
{
    return (int)fs_mkdir(path);
}

static int api_fs_remove(const char *path)
{
    return (int)fs_remove(path);
}

static int api_fs_df(uint64_t *total_bytes, uint64_t *free_bytes)
{
    return (int)fs_df(total_bytes, free_bytes);
}

static const char *api_fs_error(int error)
{
    return fat_error_string((fat_err_t)error);
}

static int api_fs_info(pieg_fsinfo_t *info)
{
    if (!info)
        return 0;

    memset(info, 0, sizeof(*info));

    if (!fs_is_mounted())
        return 0;

    info->mounted = 1;
    info->writable = fs_is_writable();
    info->pieg_volume = fs_is_pieg_volume();
    info->sectors = fs_disk_sectors();
    copy_str(info->type, fs_type_name(), sizeof(info->type));
    copy_str(info->device, fs_device_name(), sizeof(info->device));
    copy_str(info->label, fs_label(), sizeof(info->label));

    return 1;
}

static unsigned int api_blockdev_count(void)
{
    return blockdev_count();
}

static int api_blockdev_info(unsigned int index, pieg_bdinfo_t *info)
{
    blockdev_t *dev = blockdev_get(index);

    if (!dev || !info)
        return 0;

    memset(info, 0, sizeof(*info));

    copy_str(info->name, dev->name, sizeof(info->name));
    info->bytes = dev->sector_count * dev->sector_size;
    info->writable = dev->write != 0;
    info->mounted = fs_is_mounted() && str_same(dev->name, fs_device_name());

    return 1;
}

static int api_ram_info(pieg_raminfo_t *info)
{
    ramspec_t ram;

    if (!info)
        return 0;

    memset(info, 0, sizeof(*info));

    if (!get_ramspec(&ram) || !ram.initialized)
        return 0;

    info->total = ram.total_bytes;
    info->used = ram.used_bytes;
    info->free = ram.free_bytes;
    info->usage_available = ram.usage_available;

    return 1;
}

static unsigned int api_pci_count(void)
{
    return pci_device_count();
}

static int api_pci_info(unsigned int index, pieg_pciinfo_t *info)
{
    const pci_device_t *d = pci_device_get(index);

    if (!d || !info)
        return 0;

    memset(info, 0, sizeof(*info));

    info->bus = d->bus;
    info->device = d->device;
    info->function = d->function;
    info->class_code = d->class_code;
    info->subclass = d->subclass;
    info->prog_if = d->programming_interface;
    info->revision = d->revision;
    info->header_type = d->header_type;
    info->irq_line = d->irq_line;
    info->vendor_id = d->vendor_id;
    info->device_id = d->device_id;

    for (unsigned int i = 0; i < 6; i++)
        info->bar[i] = d->bar[i];

    return 1;
}

static unsigned int api_disk_count(void)
{
    return persist_disk_count();
}

static int api_disk_info(unsigned int index, pieg_diskinfo_t *info)
{
    persist_disk_t d;

    if (!info || !persist_disk_info(index, &d))
        return 0;

    memset(info, 0, sizeof(*info));

    copy_str(info->name, d.name, sizeof(info->name));
    info->bytes = d.sectors * 512u;
    info->state = d.state;
    info->free_bytes = d.free_sectors * 512u;

    return 1;
}

static const char *api_disk_install(unsigned int index, uint32_t size_mib)
{
    return persist_install(index, size_mib);
}

__attribute__((noreturn))
static void api_panic(const char *reason, const char *details)
{
    kernel_panic(reason, details);
}

__attribute__((noreturn))
static void api_reboot(void)
{
    vga_write("Rebooting PIEG...\n");
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

__attribute__((noreturn))
static void api_halt(void)
{
    vga_write("PIEG halted.\n");
    __asm__ volatile ("cli");

    for (;;)
        __asm__ volatile ("hlt");
}

static const pieg_api_t api = {
    .version = PIEG_API_VERSION,
    .shell_version = PIEG_SHELL_VERSION,

    .putchar = vga_putchar,
    .write = vga_write,
    .getchar = api_getchar,
    .clear = vga_clear,

    .cwd = shellfs_cwd,
    .resolve = api_resolve,

    .fs_list = api_fs_list,
    .fs_stat = api_fs_stat,
    .fs_read = api_fs_read,
    .fs_write = api_fs_write,
    .fs_create = api_fs_create,
    .fs_mkdir = api_fs_mkdir,
    .fs_remove = api_fs_remove,
    .fs_df = api_fs_df,
    .fs_error = api_fs_error,

    .fs_info = api_fs_info,
    .blockdev_count = api_blockdev_count,
    .blockdev_info = api_blockdev_info,
    .ram_info = api_ram_info,

    .panic = api_panic,
    .reboot = api_reboot,
    .halt = api_halt,

    .pci_count = api_pci_count,
    .pci_info = api_pci_info,

    .disk_count = api_disk_count,
    .disk_info = api_disk_info,
    .disk_install = api_disk_install,
};

/* ================================================================
 * ELF64 loader (position-independent executables)
 * ================================================================ */

#define PT_LOAD    1u
#define PT_DYNAMIC 2u

#define DT_NULL    0
#define DT_RELA    7
#define DT_RELASZ  8
#define DT_RELAENT 9

#define R_X86_64_NONE     0u
#define R_X86_64_RELATIVE 8u

typedef struct {
    uint8_t ident[16];
    uint16_t type;
    uint16_t machine;
    uint32_t version;
    uint64_t entry;
    uint64_t phoff;
    uint64_t shoff;
    uint32_t flags;
    uint16_t ehsize;
    uint16_t phentsize;
    uint16_t phnum;
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
} __attribute__((packed)) elf64_ehdr_t;

typedef struct {
    uint32_t type;
    uint32_t flags;
    uint64_t offset;
    uint64_t vaddr;
    uint64_t paddr;
    uint64_t filesz;
    uint64_t memsz;
    uint64_t align;
} __attribute__((packed)) elf64_phdr_t;

typedef struct {
    int64_t tag;
    uint64_t value;
} __attribute__((packed)) elf64_dyn_t;

typedef struct {
    uint64_t offset;
    uint64_t info;
    int64_t addend;
} __attribute__((packed)) elf64_rela_t;

#define ELF_MAX_PHDRS 16u

/* Returns NULL on success, or a message describing why loading failed. */
static const char *load_elf(const char *path, uint64_t *entry_out)
{
    uint8_t header[1024];
    uint32_t done = 0;
    fat_err_t r = fs_read(path, 0, header, sizeof(header), &done);

    if (r != FAT_OK)
        return fat_error_string(r);

    if (done < sizeof(elf64_ehdr_t))
        return "not an ELF file";

    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)header;

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L' || eh->ident[3] != 'F')
        return "not an ELF file";

    if (eh->ident[4] != 2 || eh->ident[5] != 1)
        return "not a 64-bit little-endian ELF";

    if (eh->type != 3)
        return "not position-independent (build with -pie)";

    if (eh->machine != 62)
        return "not an x86-64 program";

    if (eh->phentsize != sizeof(elf64_phdr_t) || eh->phnum == 0 ||
        eh->phnum > ELF_MAX_PHDRS)
        return "bad program headers";

    if (eh->phoff > done ||
        (uint64_t)eh->phnum * sizeof(elf64_phdr_t) > done - eh->phoff)
        return "program headers too large";

    const elf64_phdr_t *ph = (const elf64_phdr_t *)(header + eh->phoff);
    uint64_t extent = 0;

    for (unsigned int i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD)
            continue;

        if (ph[i].memsz < ph[i].filesz ||
            ph[i].vaddr >= PROGRAM_MEM_SIZE ||
            ph[i].memsz > PROGRAM_MEM_SIZE - ph[i].vaddr)
            return "program too large";

        if (ph[i].offset > 0xFFFFFFFFull)
            return "bad segment offset";

        if (ph[i].vaddr + ph[i].memsz > extent)
            extent = ph[i].vaddr + ph[i].memsz;
    }

    if (extent == 0)
        return "no loadable segments";

    if (eh->entry >= extent)
        return "bad entry point";

    memset(program_memory, 0, (size_t)extent);

    uint64_t dyn_vaddr = 0;
    uint64_t dyn_size = 0;

    for (unsigned int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_DYNAMIC) {
            dyn_vaddr = ph[i].vaddr;
            dyn_size = ph[i].memsz;
            continue;
        }

        if (ph[i].type != PT_LOAD || ph[i].filesz == 0)
            continue;

        r = fs_read(path, (uint32_t)ph[i].offset,
                    program_memory + ph[i].vaddr,
                    (uint32_t)ph[i].filesz, &done);

        if (r != FAT_OK)
            return fat_error_string(r);

        if (done != ph[i].filesz)
            return "truncated program file";
    }

    const uint64_t base = (uint64_t)(uintptr_t)program_memory;

    if (dyn_size) {
        if (dyn_vaddr >= extent || dyn_size > extent - dyn_vaddr)
            return "bad dynamic section";

        uint64_t rela = 0, rela_size = 0, rela_ent = sizeof(elf64_rela_t);

        const elf64_dyn_t *dyn =
            (const elf64_dyn_t *)(program_memory + dyn_vaddr);

        for (uint64_t i = 0; i < dyn_size / sizeof(elf64_dyn_t); i++) {
            if (dyn[i].tag == DT_NULL)
                break;
            if (dyn[i].tag == DT_RELA)
                rela = dyn[i].value;
            else if (dyn[i].tag == DT_RELASZ)
                rela_size = dyn[i].value;
            else if (dyn[i].tag == DT_RELAENT)
                rela_ent = dyn[i].value;
        }

        if (rela_size) {
            if (rela_ent != sizeof(elf64_rela_t) || rela >= extent ||
                rela_size > extent - rela)
                return "bad relocation table";

            for (uint64_t pos = 0; pos + rela_ent <= rela_size;
                 pos += rela_ent) {
                const elf64_rela_t *rel =
                    (const elf64_rela_t *)(program_memory + rela + pos);
                uint32_t type = (uint32_t)rel->info;

                if (type == R_X86_64_NONE)
                    continue;

                if (type != R_X86_64_RELATIVE)
                    return "unsupported relocation";

                if (rel->offset > extent - 8)
                    return "relocation out of range";

                uint64_t value = base + (uint64_t)rel->addend;

                memcpy(program_memory + rel->offset, &value, 8);
            }
        }
    }

    *entry_out = base + eh->entry;
    return NULL;
}

/* ================================================================
 * Lookup and execution
 * ================================================================ */

static bool is_file(const char *path)
{
    bool directory;
    uint32_t size;

    return fs_stat(path, &directory, &size) == FAT_OK && !directory;
}

static size_t str_length(const char *s)
{
    size_t n = 0;

    while (s[n])
        n++;

    return n;
}

/* Tries `base`, then `base` + ".ELF". */
static bool try_path(char *path, size_t capacity)
{
    if (is_file(path))
        return true;

    size_t n = str_length(path);

    if (n + 5 > capacity)
        return false;

    memcpy(path + n, ".ELF", 5);

    if (is_file(path))
        return true;

    path[n] = '\0';
    return false;
}

static bool find_program(const char *name, char *path, size_t capacity)
{
    bool has_slash = false;

    for (const char *p = name; *p; p++) {
        if (*p == '/') {
            has_slash = true;
            break;
        }
    }

    if (has_slash) {
        fs_resolve(shellfs_cwd(), name, path, capacity);
        return try_path(path, capacity);
    }

    size_t dir = str_length(PIEG_DIR_PROGRAMS);
    size_t len = str_length(name);

    if (dir + 1 + len + 1 > capacity)
        return false;

    memcpy(path, PIEG_DIR_PROGRAMS, dir);
    path[dir] = '/';
    memcpy(path + dir + 1, name, len + 1);

    return try_path(path, capacity);
}

typedef int (*program_entry_t)(const pieg_api_t *, int, char **);

bool program_run(int argc, char **argv, int *status)
{
    char path[FAT_PATH_MAX];
    uint64_t entry = 0;

    if (status)
        *status = 0;

    if (argc < 1 || !fs_is_mounted())
        return false;

    if (!find_program(argv[0], path, sizeof(path)))
        return false;

    const char *error = load_elf(path, &entry);

    if (error) {
        vga_write(argv[0]);
        vga_write(": cannot run ");
        vga_write(path);
        vga_write(": ");
        vga_write(error);
        vga_putchar('\n');

        if (status)
            *status = 126;

        return true;
    }

    int code = ((program_entry_t)(uintptr_t)entry)(&api, argc, argv);

    if (status)
        *status = code;

    return true;
}
