#ifndef PIEG_RAMDISK_H
#define PIEG_RAMDISK_H

#include <stdint.h>

#define RAMDISK_MAX 2

/*
 * Registers every Multiboot2 module GRUB loaded as a RAM block device
 * ("rd0", "rd1"). This is how a live medium hands the kernel its own
 * FAT volume without needing a driver for the boot device.
 * Returns the number of devices registered.
 */
unsigned int ramdisk_init(uint32_t multiboot_magic, uint32_t multiboot_info);

#endif
