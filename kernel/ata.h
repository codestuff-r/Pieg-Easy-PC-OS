#ifndef PIEG_ATA_H
#define PIEG_ATA_H

#include <stdint.h>

int ata_init(void);
int ata_read_sector(uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint32_t lba, const uint8_t *buffer);
uint32_t ata_sector_count(void);

#endif
