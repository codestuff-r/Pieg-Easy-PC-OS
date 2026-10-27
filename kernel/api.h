#ifndef PIEG_API_H
#define PIEG_API_H

/*
 * Kernel services handed to every program in /PROGRAMS.
 *
 * Programs are position-independent ELF64 executables. The shell loads one
 * and calls its entry point as:
 *
 *     int pieg_start(const pieg_api_t *api, int argc, char **argv);
 *
 * Programs run in kernel context (no privilege separation yet). They must
 * not use anything outside this table. All fs_* functions return 0 on
 * success or a nonzero error code; fs_error() turns a code into text.
 * Paths are absolute; use resolve() to turn user input into one.
 */

#include <stddef.h>
#include <stdint.h>

#define PIEG_API_VERSION    3
#define PIEG_ATTR_DIRECTORY 0x10

typedef void (*pieg_list_cb)(const char *name, uint8_t attributes,
                             uint32_t size, void *context);

typedef struct {
    int mounted;
    int writable;
    int pieg_volume;
    char type[8];
    char device[16];
    char label[16];
    uint64_t sectors;
} pieg_fsinfo_t;

typedef struct {
    char name[16];
    uint64_t bytes;
    int writable;
    int mounted;
} pieg_bdinfo_t;

typedef struct {
    uint64_t total;
    uint64_t used;
    uint64_t free;
    int usage_available;
} pieg_raminfo_t;

typedef struct {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t revision;
    uint8_t header_type;
    uint8_t irq_line;
    uint16_t vendor_id;
    uint16_t device_id;
    uint32_t bar[6];            /* raw BAR registers, 0 when absent */
} pieg_pciinfo_t;

typedef struct {
    char name[16];
    uint64_t bytes;
    int state;                  /* 0 = unsupported, 1 = blank, 2 = valid GPT */
    uint64_t free_bytes;        /* space a new PIEG volume could use */
} pieg_diskinfo_t;

typedef struct pieg_api {
    uint32_t version;
    const char *shell_version;

    /* Console. getchar() is non-blocking and returns 0 when no key is ready. */
    void (*putchar)(char c);
    void (*write)(const char *text);
    char (*getchar)(void);
    void (*clear)(void);

    /* Working directory and path handling. */
    const char *(*cwd)(void);
    void (*resolve)(const char *arg, char *out, size_t capacity);

    /* Filesystem. */
    int (*fs_list)(const char *path, pieg_list_cb callback, void *context);
    int (*fs_stat)(const char *path, int *is_directory, uint32_t *size);
    int (*fs_read)(const char *path, uint32_t offset, void *buffer,
                   uint32_t length, uint32_t *done);
    int (*fs_write)(const char *path, const void *data, uint32_t length,
                    int append);
    int (*fs_create)(const char *path);
    int (*fs_mkdir)(const char *path);
    int (*fs_remove)(const char *path);
    int (*fs_df)(uint64_t *total_bytes, uint64_t *free_bytes);
    const char *(*fs_error)(int error);

    /* System information. fs_info/ram_info return 0 when unavailable. */
    int (*fs_info)(pieg_fsinfo_t *info);
    unsigned int (*blockdev_count)(void);
    int (*blockdev_info)(unsigned int index, pieg_bdinfo_t *info);
    int (*ram_info)(pieg_raminfo_t *info);

    /* Machine control. */
    __attribute__((noreturn)) void (*panic)(const char *reason,
                                            const char *details);
    __attribute__((noreturn)) void (*reboot)(void);
    __attribute__((noreturn)) void (*halt)(void);

    /* Added in API version 2 (programs built for version 1 never touch these). */
    unsigned int (*pci_count)(void);
    int (*pci_info)(unsigned int index, pieg_pciinfo_t *info);

    /*
     * Added in API version 3: persistent storage. Disks are the physical
     * ATA/SATA disks seen at boot. disk_install() creates a PIEG volume in
     * free space (size_mib 0 = all of it, at most 4 GiB), copies the
     * running system onto it and returns NULL, or an error message.
     */
    unsigned int (*disk_count)(void);
    int (*disk_info)(unsigned int index, pieg_diskinfo_t *info);
    const char *(*disk_install)(unsigned int index, uint32_t size_mib);
} pieg_api_t;

#endif
