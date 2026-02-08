#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"

// FAT32 Boot Sector (BPB)
struct fat32_bpb {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;    // 0 for FAT32
    uint16_t total_sectors_16;    // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;         // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    // FAT32 specific
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} __attribute__((packed));

// FAT32 Directory Entry
struct fat32_dir_entry {
    uint8_t  name[11];            // 8.3 format
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_time_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed));

// Directory entry attributes
#define FAT32_ATTR_READ_ONLY 0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20
#define FAT32_ATTR_LFN       0x0F

// FAT32 filesystem state
struct fat32_fs {
    uint32_t fat_start_lba;
    uint32_t cluster_start_lba;
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t bytes_per_sector;
    uint32_t bytes_per_cluster;
    uint32_t total_clusters;
};

// Initialize FAT32 filesystem
int fat32_init(uint32_t partition_lba);

// Get root directory node
struct vfs_node *fat32_get_root(void);

// Create directory at path if missing, return its node (absolute paths only)
struct vfs_node *ensure_path_exists(const char *path);

// Create a subdirectory under parent (helper used by ensure_path_exists)
int fat32_mkdir(struct vfs_node *parent, const char *name);

// Minimal stubs for file operations (not fully implemented)
struct vfs_node *fat32_create_file(struct vfs_node *parent, const char *name);
int fat32_unlink(struct vfs_node *parent, const char *name);
int fat32_rmdir(struct vfs_node *parent, const char *name);
int fat32_rename(struct vfs_node *old_parent, const char *old_name,
                 struct vfs_node *new_parent, const char *new_name);
int fat32_truncate(struct vfs_node *node, int size);
int fat32_flush_size(struct vfs_node *node);

// Path-based helpers for userland commands (rm, rmdir, touch, ls, mv)
int fat32_touch_path(const char *path);
int fat32_rm_path(const char *path);
int fat32_rmdir_path(const char *path);
int fat32_mv_path(const char *src, const char *dst);
int fat32_ls_path(const char *path,
                  int (*visitor)(const struct dirent *de, void *ctx),
                  void *ctx);

#endif
