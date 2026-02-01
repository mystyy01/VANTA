#include "fat32.h"
#include "../drivers/ata.h"

// Filesystem state
static struct fat32_fs fs;
static struct vfs_node root_node;
static uint8_t sector_buffer[512];
static uint8_t cluster_buffer[4096];  // Max 4KB cluster

// Directory entry buffer
static struct dirent dirent_buf;

// Node cache (simple, fixed size)
#define NODE_CACHE_SIZE 32
static struct vfs_node node_cache[NODE_CACHE_SIZE];
static int node_cache_used = 0;

// String functions
static int strlen(const char *s) {
    int len = 0;
    while (*s++) len++;
    return len;
}

static void memcpy(void *dest, const void *src, uint32_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;
    while (n--) *d++ = *s++;
}

static void memset(void *dest, uint8_t val, uint32_t n) {
    uint8_t *d = dest;
    while (n--) *d++ = val;
}

static int strncmp(const char *a, const char *b, int n) {
    while (n-- && *a && *b) {
        if (*a != *b) return *a - *b;
        a++;
        b++;
    }
    return 0;
}

// Convert cluster number to LBA
static uint32_t cluster_to_lba(uint32_t cluster) {
    return fs.cluster_start_lba + (cluster - 2) * fs.sectors_per_cluster;
}

// Read a cluster
static int read_cluster(uint32_t cluster, void *buffer) {
    uint32_t lba = cluster_to_lba(cluster);
    return ata_read_sectors(lba, fs.sectors_per_cluster, buffer);
}

// Get next cluster from FAT
static uint32_t get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs.fat_start_lba + (fat_offset / fs.bytes_per_sector);
    uint32_t entry_offset = fat_offset % fs.bytes_per_sector;

    ata_read_sectors(fat_sector, 1, sector_buffer);

    uint32_t next = *(uint32_t *)(sector_buffer + entry_offset);
    next &= 0x0FFFFFFF;  // Mask off high 4 bits

    return next;
}

// Check if cluster is end of chain
static int is_end_of_chain(uint32_t cluster) {
    return cluster >= 0x0FFFFFF8;
}

// Convert 8.3 filename to normal string
static void fat32_name_to_string(const uint8_t *fat_name, char *out) {
    int i, j = 0;

    // Copy name (first 8 chars, trim spaces)
    for (i = 0; i < 8 && fat_name[i] != ' '; i++) {
        out[j++] = fat_name[i];
    }

    // Add extension if present
    if (fat_name[8] != ' ') {
        out[j++] = '.';
        for (i = 8; i < 11 && fat_name[i] != ' '; i++) {
            out[j++] = fat_name[i];
        }
    }

    out[j] = 0;

    // Convert to lowercase
    for (i = 0; out[i]; i++) {
        if (out[i] >= 'A' && out[i] <= 'Z') {
            out[i] += 32;
        }
    }
}

// Convert string to 8.3 filename
static void string_to_fat32_name(const char *str, uint8_t *fat_name) {
    int i, j = 0;

    memset(fat_name, ' ', 11);

    // Copy name part
    for (i = 0; str[i] && str[i] != '.' && j < 8; i++) {
        char c = str[i];
        if (c >= 'a' && c <= 'z') c -= 32;  // To uppercase
        fat_name[j++] = c;
    }

    // Skip to extension
    while (str[i] && str[i] != '.') i++;
    if (str[i] == '.') i++;

    // Copy extension
    j = 8;
    while (str[i] && j < 11) {
        char c = str[i++];
        if (c >= 'a' && c <= 'z') c -= 32;
        fat_name[j++] = c;
    }
}

// Forward declarations
static int fat32_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static struct dirent *fat32_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name);

// Allocate a node from cache
static struct vfs_node *alloc_node(void) {
    if (node_cache_used < NODE_CACHE_SIZE) {
        return &node_cache[node_cache_used++];
    }
    return 0;  // Cache full
}

// Create a VFS node from directory entry
static struct vfs_node *create_node(struct fat32_dir_entry *entry) {
    struct vfs_node *node = alloc_node();
    if (!node) return 0;

    fat32_name_to_string(entry->name, node->name);

    uint32_t cluster = (entry->first_cluster_high << 16) | entry->first_cluster_low;

    node->inode = cluster;
    node->size = entry->file_size;
    node->private_data = 0;

    if (entry->attr & FAT32_ATTR_DIRECTORY) {
        node->flags = VFS_DIRECTORY;
        node->read = 0;
        node->write = 0;
        node->readdir = fat32_readdir;
        node->finddir = fat32_finddir;
    } else {
        node->flags = VFS_FILE;
        node->read = fat32_read;
        node->write = 0;  // Read-only for now
        node->readdir = 0;
        node->finddir = 0;
    }

    return node;
}

// Read file contents
static int fat32_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !(node->flags & VFS_FILE)) return -1;

    uint32_t cluster = node->inode;
    uint32_t bytes_read = 0;
    uint32_t file_pos = 0;

    // Skip to offset cluster
    while (file_pos + fs.bytes_per_cluster <= offset && !is_end_of_chain(cluster)) {
        file_pos += fs.bytes_per_cluster;
        cluster = get_next_cluster(cluster);
    }

    // Read data
    while (bytes_read < size && !is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);

        uint32_t cluster_offset = 0;
        if (file_pos < offset) {
            cluster_offset = offset - file_pos;
        }

        uint32_t to_copy = fs.bytes_per_cluster - cluster_offset;
        if (to_copy > size - bytes_read) {
            to_copy = size - bytes_read;
        }
        if (file_pos + cluster_offset + to_copy > node->size) {
            to_copy = node->size - file_pos - cluster_offset;
        }

        memcpy(buffer + bytes_read, cluster_buffer + cluster_offset, to_copy);
        bytes_read += to_copy;
        file_pos += fs.bytes_per_cluster;

        cluster = get_next_cluster(cluster);
    }

    return bytes_read;
}

// Read directory entry by index
static struct dirent *fat32_readdir(struct vfs_node *node, uint32_t index) {
    if (!node || !(node->flags & VFS_DIRECTORY)) return 0;

    uint32_t cluster = node->inode;
    uint32_t entry_index = 0;

    while (!is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);

        int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

        for (int i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *entry = &entries[i];

            // End of directory
            if (entry->name[0] == 0x00) return 0;

            // Skip deleted entries
            if (entry->name[0] == 0xE5) continue;

            // Skip LFN entries
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;

            // Skip volume label
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;

            // Skip . and ..
            if (entry->name[0] == '.') continue;

            if (entry_index == index) {
                fat32_name_to_string(entry->name, dirent_buf.name);
                dirent_buf.inode = (entry->first_cluster_high << 16) | entry->first_cluster_low;
                return &dirent_buf;
            }

            entry_index++;
        }

        cluster = get_next_cluster(cluster);
    }

    return 0;
}

// Find file/directory by name
static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name) {
    if (!node || !(node->flags & VFS_DIRECTORY)) return 0;

    uint8_t fat_name[11];
    string_to_fat32_name(name, fat_name);

    uint32_t cluster = node->inode;

    while (!is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);

        int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

        for (int i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *entry = &entries[i];

            // End of directory
            if (entry->name[0] == 0x00) return 0;

            // Skip deleted entries
            if (entry->name[0] == 0xE5) continue;

            // Skip LFN entries
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;

            // Skip volume label
            if (entry->attr & FAT32_ATTR_VOLUME_ID) continue;

            // Check name match
            if (strncmp((char *)entry->name, (char *)fat_name, 11) == 0) {
                return create_node(entry);
            }
        }

        cluster = get_next_cluster(cluster);
    }

    return 0;
}

int fat32_init(uint32_t partition_lba) {
    // Read boot sector
    ata_read_sectors(partition_lba, 1, sector_buffer);

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector_buffer;

    // Verify it's FAT32
    if (bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) {
        return -1;  // Not FAT32
    }

    // Store filesystem info
    fs.bytes_per_sector = bpb->bytes_per_sector;
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.bytes_per_cluster = fs.bytes_per_sector * fs.sectors_per_cluster;
    fs.fat_start_lba = partition_lba + bpb->reserved_sectors;
    fs.cluster_start_lba = fs.fat_start_lba + (bpb->num_fats * bpb->fat_size_32);
    fs.root_cluster = bpb->root_cluster;

    // Set up root node
    memset(&root_node, 0, sizeof(root_node));
    root_node.name[0] = '/';
    root_node.name[1] = 0;
    root_node.flags = VFS_DIRECTORY;
    root_node.inode = fs.root_cluster;
    root_node.readdir = fat32_readdir;
    root_node.finddir = fat32_finddir;

    return 0;
}

struct vfs_node *fat32_get_root(void) {
    return &root_node;
}
