#ifndef ATA_H
#define ATA_H

#include <stdint.h>

// ATA ports (primary bus)
#define ATA_PRIMARY_DATA         0x1F0
#define ATA_PRIMARY_ERROR        0x1F1
#define ATA_PRIMARY_SECTOR_COUNT 0x1F2
#define ATA_PRIMARY_LBA_LOW      0x1F3
#define ATA_PRIMARY_LBA_MID      0x1F4
#define ATA_PRIMARY_LBA_HIGH     0x1F5
#define ATA_PRIMARY_DRIVE_SELECT 0x1F6
#define ATA_PRIMARY_STATUS       0x1F7
#define ATA_PRIMARY_COMMAND      0x1F7

// ATA commands
#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_IDENTIFY      0xEC

// ATA status bits
#define ATA_STATUS_BSY  0x80  // Busy
#define ATA_STATUS_DRDY 0x40  // Drive ready
#define ATA_STATUS_DRQ  0x08  // Data request
#define ATA_STATUS_ERR  0x01  // Error

// Drive selection
#define ATA_DRIVE_MASTER 0
#define ATA_DRIVE_SLAVE  1

void ata_init(void);
void ata_select_drive(int drive);
int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer);
int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer);

#endif
