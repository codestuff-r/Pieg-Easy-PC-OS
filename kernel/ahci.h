#ifndef PIEG_AHCI_H
#define PIEG_AHCI_H

#include "blockdev.h"

#define AHCI_MAX_DISKS 4

/*
 * Minimal SATA driver for AHCI controllers (PCI class 01:06:01).
 * Polled, one command slot, 48-bit LBA DMA, 512-byte sectors only.
 *
 * ahci_init() finds the controller(s), brings up every port that has a
 * SATA disk attached and returns how many disks it found. The disks are
 * NOT registered with blockdev: fsinit does that (it also scans their
 * partitions). Disk i is named "sd<i>".
 */
unsigned int ahci_init(void);
unsigned int ahci_disk_count(void);
blockdev_t *ahci_disk_get(unsigned int index);

#endif
