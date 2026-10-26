#ifndef PIEG_FSINIT_H
#define PIEG_FSINIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fat.h"

/*
 * PIEG volume layout (a FAT volume labelled "PIEG"):
 *
 *   /#          kernel and system files
 *   /PROGRAMS   programs
 *   /U/HOME     user home directory
 *
 * Names are case-insensitive: /programs and /u/home work.
 */
#define PIEG_DIR_SYSTEM   "/#"
#define PIEG_DIR_PROGRAMS "/PROGRAMS"
#define PIEG_DIR_USERS    "/U"
#define PIEG_DIR_HOME     "/U/HOME"
#define PIEG_VOLUME_LABEL "PIEG"

void fsinit(uint32_t multiboot_magic, uint32_t multiboot_info);

bool fs_is_mounted(void);
bool fs_is_writable(void);
bool fs_is_pieg_volume(void);
uint64_t fs_disk_sectors(void);
const char *fs_type_name(void);
const char *fs_device_name(void);
const char *fs_label(void);

typedef void (*fs_list_callback_t)(
    const char *name,
    uint8_t attributes,
    uint32_t size,
    void *context
);

fat_err_t fs_list(const char *path, fs_list_callback_t callback,
                  void *context);
fat_err_t fs_stat(const char *path, bool *is_directory, uint32_t *size);
fat_err_t fs_read(const char *path, uint32_t offset, void *buffer,
                  uint32_t length, uint32_t *done);
fat_err_t fs_write(const char *path, const void *data, uint32_t length,
                   bool append);
fat_err_t fs_create(const char *path);
fat_err_t fs_mkdir(const char *path);
fat_err_t fs_remove(const char *path);
fat_err_t fs_df(uint64_t *total_bytes, uint64_t *free_bytes);

/* Joins cwd and arg, expands "~", resolves "." and "..". */
void fs_resolve(const char *cwd, const char *arg, char *out, size_t capacity);

#endif
