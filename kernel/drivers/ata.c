#include "ata.h"

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

static inline void inw_rep(uint16_t port, void *addr, uint32_t count) {
    __asm__ volatile ("rep insw" : "+D"(addr), "+c"(count) : "d"(port) : "memory");
}

static inline void outw_rep(uint16_t port, const void *addr, uint32_t count) {
    __asm__ volatile ("rep outsw" : "+S"(addr), "+c"(count) : "d"(port));
}

static void ata_wait_ready(void) {
    while (inb(ATA_PRIMARY_STATUS) & ATA_STATUS_BSY);
}

static void ata_wait_drq(void) {
    while (!(inb(ATA_PRIMARY_STATUS) & ATA_STATUS_DRQ));
}

static int current_drive = 0;

void ata_init(void) {
    // Select primary master drive
    outb(ATA_PRIMARY_DRIVE_SELECT, 0xA0);
    current_drive = 0;

    // Small delay (read status port 4 times)
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_STATUS);
    }
}

void ata_select_drive(int drive) {
    current_drive = drive;
    // 0xA0 for master, 0xB0 for slave
    outb(ATA_PRIMARY_DRIVE_SELECT, drive ? 0xB0 : 0xA0);

    // Small delay
    for (int i = 0; i < 4; i++) {
        inb(ATA_PRIMARY_STATUS);
    }
}

int ata_read_sectors(uint32_t lba, uint8_t count, void *buffer) {
    if (count == 0 || !buffer) return -1;
    ata_wait_ready();

    // Select drive and set high LBA bits (0xE0 for master, 0xF0 for slave)
    uint8_t drive_bits = current_drive ? 0xF0 : 0xE0;
    outb(ATA_PRIMARY_DRIVE_SELECT, drive_bits | ((lba >> 24) & 0x0F));

    // Set sector count
    outb(ATA_PRIMARY_SECTOR_COUNT, count);

    // Set LBA address
    outb(ATA_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);

    // Send read command
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_READ_SECTORS);

    // Read sectors
    uint16_t *buf = (uint16_t *)buffer;
    for (int i = 0; i < count; i++) {
        ata_wait_drq();
        inw_rep(ATA_PRIMARY_DATA, buf, 256);  // 256 words = 512 bytes
        buf += 256;
    }

    return 0;
}

int ata_write_sectors(uint32_t lba, uint8_t count, const void *buffer) {
    if (count == 0 || !buffer) return -1;
    ata_wait_ready();

    // Select drive and set high LBA bits (0xE0 for master, 0xF0 for slave)
    uint8_t drive_bits = current_drive ? 0xF0 : 0xE0;
    outb(ATA_PRIMARY_DRIVE_SELECT, drive_bits | ((lba >> 24) & 0x0F));

    // Set sector count
    outb(ATA_PRIMARY_SECTOR_COUNT, count);

    // Set LBA address
    outb(ATA_PRIMARY_LBA_LOW, lba & 0xFF);
    outb(ATA_PRIMARY_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_PRIMARY_LBA_HIGH, (lba >> 16) & 0xFF);

    // Send write command
    outb(ATA_PRIMARY_COMMAND, ATA_CMD_WRITE_SECTORS);

    // Write sectors
    const uint16_t *buf = (const uint16_t *)buffer;
    for (int i = 0; i < count; i++) {
        ata_wait_drq();
        outw_rep(ATA_PRIMARY_DATA, buf, 256);
        buf += 256;
    }

    return 0;
}
