#include "fat32.h"
#include "../drivers/ata.h"

// Filesystem state
static struct fat32_fs fs;
static struct vfs_node root_node;
static uint8_t sector_buffer[512];
#define CLUSTER_BUFFER_SIZE 4096
static uint8_t cluster_buffer[CLUSTER_BUFFER_SIZE];  // Max 4KB cluster

// Directory entry buffer
static struct dirent dirent_buf;

// Node cache (simple, fixed size)
#define NODE_CACHE_SIZE 64
static struct vfs_node node_cache[NODE_CACHE_SIZE];
static int node_cache_used = 0;

// Error codes (negative to signal failure)
#define FAT32_E_OK        0
#define FAT32_E_NOENT    -2
#define FAT32_E_EXIST    -3
#define FAT32_E_NOTDIR   -4
#define FAT32_E_ISDIR    -5
#define FAT32_E_NOTEMPTY -6
#define FAT32_E_INVAL    -8
#define FAT32_E_NOSPC    -9

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

static int strcmp_local(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

static int is_special_name(const char *name) {
    if (name[0] == '.' && name[1] == '\0') return 1;
    if (name[0] == '.' && name[1] == '.' && name[2] == '\0') return 1;
    return 0;
}

// Convert cluster number to LBA
static uint32_t cluster_to_lba(uint32_t cluster) {
    return fs.cluster_start_lba + (cluster - 2) * fs.sectors_per_cluster;
}

// Read a cluster
static int read_cluster(uint32_t cluster, void *buffer) {
    if (fs.sectors_per_cluster == 0 || fs.bytes_per_cluster == 0) return FAT32_E_INVAL;
    if (fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) return FAT32_E_INVAL;
    uint32_t lba = cluster_to_lba(cluster);
    return ata_read_sectors(lba, fs.sectors_per_cluster, buffer);
}

static int write_cluster(uint32_t cluster, void *buffer) {
    if (fs.sectors_per_cluster == 0 || fs.bytes_per_cluster == 0) return FAT32_E_INVAL;
    if (fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) return FAT32_E_INVAL;
    uint32_t lba = cluster_to_lba(cluster);
    return ata_write_sectors(lba, fs.sectors_per_cluster, buffer);
}

// Get next cluster from FAT
static uint32_t get_next_cluster(uint32_t cluster) {
    uint32_t fat_offset = cluster * 4; // get byte offset
    uint32_t fat_sector = fs.fat_start_lba + (fat_offset / fs.bytes_per_sector); // find the sector using integer division to round down to the nearest sector
    uint32_t entry_offset = fat_offset % fs.bytes_per_sector; // use modulo to get the clusters offset in the sector worked out in the previous calculation

    ata_read_sectors(fat_sector, 1, sector_buffer);

    uint32_t next = *(uint32_t *)(sector_buffer + entry_offset);
    next &= 0x0FFFFFFF;  // Mask off high 4 bits

    return next;
}

static int set_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_offset = cluster * 4;
    uint32_t fat_sector = fs.fat_start_lba + (fat_offset / fs.bytes_per_sector);
    uint32_t entry_offset = fat_offset % fs.bytes_per_sector;

    ata_read_sectors(fat_sector, 1, sector_buffer); // read to stop garbage memory in sector_buffer

    uint32_t *ptr = (uint32_t *)(sector_buffer + entry_offset); // cast pointer to a 4 byte type at the position of the 4 byte write in terms of the whole disk, not just the cluster
    *ptr = value; // set 4 byte *ptr to value

    ata_write_sectors(fat_sector, 1, sector_buffer); // write the sector back to disk

    return 0;
}

static uint32_t find_free_cluster(){
    for (int i = 2; i < fs.total_clusters; i++){ 
        uint32_t entry = get_next_cluster(i);
        if (entry == 0x00000000){ // check if the cluster is empty
            return i; 
        }
    }
    return 0;
}

// Forward declaration
static void string_to_fat32_name(const char *str, uint8_t *fat_name);
static int fat32_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
static int fat32_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
static struct dirent *fat32_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name);
static int is_end_of_chain(uint32_t cluster);

static void free_cluster_chain(uint32_t cluster) {
    if (cluster < 2) return;
    if (cluster == fs.root_cluster) return; // never free root cluster
    while (!is_end_of_chain(cluster) && cluster != 0) {
        uint32_t next = get_next_cluster(cluster);
        set_fat_entry(cluster, 0x00000000);
        if (next == cluster) break; // safety: avoid loops
        cluster = next;
    }
    if (!is_end_of_chain(cluster) && cluster >= 2) {
        set_fat_entry(cluster, 0x00000000);
    }
}

static uint32_t alloc_cluster_zeroed(void) {
    uint32_t cl = find_free_cluster();
    if (cl == 0) return 0;
    set_fat_entry(cl, 0x0FFFFFFF);
    memset(cluster_buffer, 0, fs.bytes_per_cluster);
    write_cluster(cl, cluster_buffer);
    return cl;
}

static int append_cluster(uint32_t head_cluster, uint32_t new_cluster) {
    if (head_cluster < 2 || new_cluster < 2) return FAT32_E_INVAL;

    uint32_t current = head_cluster;
    uint32_t guard = 0;
    while (!is_end_of_chain(get_next_cluster(current))) {
        current = get_next_cluster(current);
        guard++;
        if (guard > fs.total_clusters) return FAT32_E_INVAL; // prevent loop
    }

    set_fat_entry(current, new_cluster);
    set_fat_entry(new_cluster, 0x0FFFFFFF);
    return FAT32_E_OK;
}

// Find a directory entry by FAT short name within a directory node.
// Returns pointer into cluster_buffer for in-place edits.
static int find_entry_in_dir(struct vfs_node *dir, const uint8_t fat_name[11],
                             struct fat32_dir_entry **out_entry,
                             uint32_t *out_cluster) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;
    uint32_t cluster = dir->inode;

    while (!is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);

        int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

        for (int i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *entry = &entries[i];

            if (entry->name[0] == 0x00) return FAT32_E_NOENT; // end marker
            if (entry->name[0] == 0xE5) continue;             // deleted
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;
            if (strncmp((char *)entry->name, (char *)fat_name, 11) == 0) {
                *out_entry = entry;
                *out_cluster = cluster;
                return FAT32_E_OK;
            }
        }
        cluster = get_next_cluster(cluster);
    }
    return FAT32_E_NOENT;
}

// Locate a free/deleted slot in a directory, extending the directory if needed.
static int ensure_dir_slot(struct vfs_node *dir,
                           struct fat32_dir_entry **out_entry,
                           uint32_t *out_cluster) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;

    uint32_t cluster = dir->inode;

    while (!is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);
        int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

        for (int i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *e = &entries[i];
            if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                *out_entry = e;
                *out_cluster = cluster;
                return FAT32_E_OK;
            }
        }
        uint32_t next = get_next_cluster(cluster);
        if (is_end_of_chain(next)) break;
        cluster = next;
    }

    // Need to extend directory
    uint32_t new_cluster = alloc_cluster_zeroed();
    if (new_cluster == 0) return FAT32_E_NOSPC;
    if (append_cluster(cluster, new_cluster) != FAT32_E_OK) {
        set_fat_entry(new_cluster, 0); // rollback allocation
        return FAT32_E_INVAL;
    }
    read_cluster(new_cluster, cluster_buffer); // zeroed but ensure buffer
    *out_cluster = new_cluster;
    *out_entry = (struct fat32_dir_entry *)cluster_buffer; // first entry
    return FAT32_E_OK;
}

// Split path into parent path and leaf (last component)
static int split_path(const char *path, char *parent_out, char *leaf_out) {
    if (!path || !parent_out || !leaf_out) return FAT32_E_INVAL;
    int len = strlen(path);
    if (len == 0) return FAT32_E_INVAL;

    // Find last '/'
    int last_slash = -1;
    for (int i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = i;
    }

    if (last_slash <= 0) {
        // Parent is root
        parent_out[0] = '/';
        parent_out[1] = '\0';
        int j = 0;
        for (int i = (path[0] == '/' ? 1 : 0); path[i] && j < VFS_MAX_NAME - 1; i++) {
            leaf_out[j++] = path[i];
        }
        leaf_out[j] = '\0';
    } else {
        // Copy parent
        int p_len = (last_slash == 0) ? 1 : last_slash;
        if (p_len >= VFS_MAX_PATH) return FAT32_E_INVAL;
        for (int i = 0; i < p_len; i++) parent_out[i] = path[i];
        parent_out[p_len] = '\0';

        int j = 0;
        for (int i = last_slash + 1; path[i] && j < VFS_MAX_NAME - 1; i++) {
            leaf_out[j++] = path[i];
        }
        leaf_out[j] = '\0';
    }
    if (leaf_out[0] == '\0') return FAT32_E_INVAL;
    return FAT32_E_OK;
}

static int dir_is_empty(struct vfs_node *dir) {
    if (!dir || !(dir->flags & VFS_DIRECTORY)) return 0;
    uint32_t idx = 0;
    struct dirent *d;
    while ((d = vfs_readdir(dir, idx++)) != 0) {
        // fat32_readdir already skips . and ..
        return 0; // found a real entry
    }
    return 1;
}

int fat32_mkdir(struct vfs_node *parent, const char *name){
    uint32_t allocated_cluster = find_free_cluster(); // find free space to put the new directory
    if (allocated_cluster == 0) return -1; // disk full
    set_fat_entry(allocated_cluster, 0x0FFFFFFF); // mark as end of chain
    memset(cluster_buffer, 0, fs.bytes_per_cluster); // set the cluster buffer to all 0s to initialize
    struct fat32_dir_entry *dot = (struct fat32_dir_entry *)cluster_buffer; // treat the start of the cluster buffer as a directory entry (.)
    struct fat32_dir_entry *dotdot = (struct fat32_dir_entry *)(cluster_buffer + 32);

    // adding . entry
    memcpy(dot->name, ".          ", 11);
    dot->attr = 0x10;
    dot->first_cluster_low = allocated_cluster & 0xFFFF;
    dot->first_cluster_high = (allocated_cluster >> 16) & 0xFFFF;

    // adding .. entry
    memcpy(dotdot->name, "..         ", 11);
    dotdot->attr = 0x10;
    dotdot->first_cluster_low = parent->inode & 0xFFFF;
    dotdot->first_cluster_high = (parent->inode >> 16) & 0xFFFF;

    write_cluster(allocated_cluster, cluster_buffer);

    read_cluster(parent->inode, cluster_buffer);
    
    int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);

    struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

    for (int i = 0; i < entries_per_cluster; i++){
        if (entries[i].name[0] == 0x00){ // found an empty slot for the new directory
            string_to_fat32_name(name, entries[i].name);
            
            entries[i].attr = 0x10;
            entries[i].first_cluster_low = allocated_cluster & 0xFFFF;
            entries[i].first_cluster_high = (allocated_cluster >> 16) & 0xFFFF;
            entries[i].file_size = 0;

            break;
        }
    }
    write_cluster(parent->inode, cluster_buffer);
    return 0;

}

// Create empty file entry in parent directory
struct vfs_node *fat32_create_file(struct vfs_node *parent, const char *name) {
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !name) return 0;
    if (is_special_name(name)) return 0;

    uint8_t fat_name[11];
    string_to_fat32_name(name, fat_name);

    // Fail if it already exists
    struct vfs_node *existing = fat32_finddir(parent, name);
    if (existing) return 0;

    struct fat32_dir_entry *slot;
    uint32_t slot_cluster;
    if (ensure_dir_slot(parent, &slot, &slot_cluster) != FAT32_E_OK) return 0;

    memcpy(slot->name, fat_name, 11);
    slot->attr = FAT32_ATTR_ARCHIVE;
    slot->first_cluster_low = 0;
    slot->first_cluster_high = 0;
    slot->file_size = 0;

    write_cluster(slot_cluster, cluster_buffer);
    struct vfs_node *node = fat32_finddir(parent, name);
    if (node) {
        node->private_data = (void *)(uintptr_t)parent->inode;
    }
    return node;
}

int fat32_unlink(struct vfs_node *parent, const char *name) {
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !name) return FAT32_E_INVAL;
    if (is_special_name(name)) return FAT32_E_INVAL;

    uint8_t fat_name[11];
    string_to_fat32_name(name, fat_name);

    struct fat32_dir_entry *entry;
    uint32_t entry_cluster;
    int st = find_entry_in_dir(parent, fat_name, &entry, &entry_cluster);
    if (st != FAT32_E_OK) return st;

    if (entry->attr & FAT32_ATTR_DIRECTORY) return FAT32_E_ISDIR;

    // NOTE: to avoid filesystem corruption seen during testing, we do NOT
    // free the cluster chain here yet. Only mark the directory entry deleted.
    // uint32_t first_cluster = (entry->first_cluster_high << 16) | entry->first_cluster_low;
    // if (first_cluster >= 2 && first_cluster != fs.root_cluster) free_cluster_chain(first_cluster);

    entry->name[0] = 0xE5; // mark deleted
    write_cluster(entry_cluster, cluster_buffer);
    return FAT32_E_OK;
}

int fat32_rmdir(struct vfs_node *parent, const char *name) {
    if (!parent || !(parent->flags & VFS_DIRECTORY) || !name) return FAT32_E_INVAL;
    if (is_special_name(name)) return FAT32_E_INVAL;

    uint8_t fat_name[11];
    string_to_fat32_name(name, fat_name);

    struct fat32_dir_entry *entry;
    uint32_t entry_cluster;
    int st = find_entry_in_dir(parent, fat_name, &entry, &entry_cluster);
    if (st != FAT32_E_OK) return st;

    if (!(entry->attr & FAT32_ATTR_DIRECTORY)) return FAT32_E_NOTDIR;

    struct vfs_node *dir_node = fat32_finddir(parent, name);
    if (!dir_node) return FAT32_E_NOENT;
    if (!dir_is_empty(dir_node)) return FAT32_E_NOTEMPTY;

    // Re-read parent cluster to restore cluster_buffer before modifying entry.
    struct fat32_dir_entry *entry_refresh;
    uint32_t cluster_refresh;
    int st2 = find_entry_in_dir(parent, fat_name, &entry_refresh, &cluster_refresh);
    if (st2 != FAT32_E_OK) return st2;

    // Free the directory's cluster chain (but never root)
    uint32_t first_cluster = (entry_refresh->first_cluster_high << 16) | entry_refresh->first_cluster_low;
    if (first_cluster >= 2 && first_cluster != fs.root_cluster) {
        free_cluster_chain(first_cluster);
    }

    entry_refresh->name[0] = 0xE5;
    write_cluster(cluster_refresh, cluster_buffer);
    return FAT32_E_OK;
}

int fat32_rename(struct vfs_node *old_parent, const char *old_name,
                 struct vfs_node *new_parent, const char *new_name) {
    if (!old_parent || !new_parent || !old_name || !new_name) return FAT32_E_INVAL;
    if (!(old_parent->flags & VFS_DIRECTORY) || !(new_parent->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;
    if (is_special_name(old_name) || is_special_name(new_name)) return FAT32_E_INVAL;

    uint8_t old_fat[11], new_fat[11];
    string_to_fat32_name(old_name, old_fat);
    string_to_fat32_name(new_name, new_fat);

    // Destination must not exist
    if (fat32_finddir(new_parent, new_name)) return FAT32_E_EXIST;

    // Find source entry
    struct fat32_dir_entry *entry;
    uint32_t entry_cluster;
    int st = find_entry_in_dir(old_parent, old_fat, &entry, &entry_cluster);
    if (st != FAT32_E_OK) return st;

    uint8_t attr = entry->attr;
    uint32_t first_cluster = (entry->first_cluster_high << 16) | entry->first_cluster_low;
    uint32_t size = entry->file_size;

    // Remove source entry
    entry->name[0] = 0xE5;
    write_cluster(entry_cluster, cluster_buffer);

    // Insert new entry
    struct fat32_dir_entry *dst_slot;
    uint32_t dst_cluster;
    st = ensure_dir_slot(new_parent, &dst_slot, &dst_cluster);
    if (st != FAT32_E_OK) {
        // rollback
        entry->name[0] = old_fat[0];
        memcpy(entry->name, old_fat, 11);
        write_cluster(entry_cluster, cluster_buffer);
        return st;
    }

    memcpy(dst_slot->name, new_fat, 11);
    dst_slot->attr = attr;
    dst_slot->first_cluster_low = first_cluster & 0xFFFF;
    dst_slot->first_cluster_high = (first_cluster >> 16) & 0xFFFF;
    dst_slot->file_size = size;
    write_cluster(dst_cluster, cluster_buffer);

    // If moving a directory, update its ".." to point to new parent
    if (attr & FAT32_ATTR_DIRECTORY && first_cluster >= 2) {
        read_cluster(first_cluster, cluster_buffer);
        struct fat32_dir_entry *dotdot = (struct fat32_dir_entry *)(cluster_buffer + 32);
        dotdot->first_cluster_low = new_parent->inode & 0xFFFF;
        dotdot->first_cluster_high = (new_parent->inode >> 16) & 0xFFFF;
        write_cluster(first_cluster, cluster_buffer);
    }

    return FAT32_E_OK;
}

int fat32_truncate(struct vfs_node *node, int size) {
    if (!node || !(node->flags & VFS_FILE)) return FAT32_E_INVAL;
    if (size == 0) {
        if (node->inode >= 2) free_cluster_chain(node->inode);
        node->inode = 0;
        node->size = 0;
    }
    return FAT32_E_OK;
}

// Ensure an absolute directory path exists, creating intermediate dirs.
struct vfs_node *ensure_path_exists(const char *path) {
    if (!path || !*path) return 0;
    if (!fs.cluster_start_lba) return 0; // FAT not initialised

    struct vfs_node *current = &root_node;

    // Skip leading slash
    const char *p = path;
    if (*p == '/') p++;

    char component[VFS_MAX_NAME];
    while (*p) {
        int len = 0;
        while (*p && *p != '/' && len < VFS_MAX_NAME - 1) {
            component[len++] = *p++;
        }
        component[len] = 0;
        if (*p == '/') p++;
        if (len == 0) continue;

        struct vfs_node *child = vfs_finddir(current, component);
        if (!child) {
            if (fat32_mkdir(current, component) != 0) return 0;
            child = vfs_finddir(current, component);
            if (!child) return 0;
        }
        if (!(child->flags & VFS_DIRECTORY)) return 0;
        current = child;
    }
    return current;
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
static int fat32_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
static struct dirent *fat32_readdir(struct vfs_node *node, uint32_t index);
static struct vfs_node *fat32_finddir(struct vfs_node *node, const char *name);

// Allocate a node from cache
static struct vfs_node *alloc_node(void) {
    if (node_cache_used < NODE_CACHE_SIZE) {
        return &node_cache[node_cache_used++];
    }
    return 0;  // Cache full
}

// Create a VFS node from directory entry (with cache deduplication)
static struct vfs_node *create_node(struct fat32_dir_entry *entry) {
    uint32_t cluster = (entry->first_cluster_high << 16) | entry->first_cluster_low;

    // Reuse existing node if same cluster is already cached
    if (cluster >= 2) {
        for (int i = 0; i < node_cache_used; i++) {
            if (node_cache[i].inode == cluster) {
                // Update with latest on-disk info
                fat32_name_to_string(entry->name, node_cache[i].name);
                node_cache[i].size = entry->file_size;
                return &node_cache[i];
            }
        }
    }

    struct vfs_node *node = alloc_node();
    if (!node) return 0;

    fat32_name_to_string(entry->name, node->name);

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
        node->write = fat32_write;
        node->readdir = 0;
        node->finddir = 0;
    }

    return node;
}

// Read file contents
static int fat32_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (!node || !(node->flags & VFS_FILE)) return -1;
    if (fs.bytes_per_cluster == 0 || fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) return -1;

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

// Write file contents
static int fat32_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer) {
    if (!node || !(node->flags & VFS_FILE)) return -1;
    if (size == 0) return 0;
    if (fs.bytes_per_cluster == 0 || fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) return -1;

    uint32_t cluster = node->inode;
    uint32_t bytes_written = 0;
    uint32_t file_pos = 0;

    // If the file has no clusters yet, allocate the first one
    if (cluster < 2) {
        cluster = alloc_cluster_zeroed();
        if (cluster == 0) return -1;
        node->inode = cluster;
    }

    // Walk cluster chain to the cluster containing offset
    while (file_pos + fs.bytes_per_cluster <= offset) {
        uint32_t next = get_next_cluster(cluster);
        if (is_end_of_chain(next)) {
            // Need to extend: allocate a new cluster
            uint32_t new_cl = alloc_cluster_zeroed();
            if (new_cl == 0) return bytes_written > 0 ? (int)bytes_written : -1;
            append_cluster(cluster, new_cl);
            next = new_cl;
        }
        file_pos += fs.bytes_per_cluster;
        cluster = next;
    }

    // Write data cluster by cluster
    while (bytes_written < size) {
        read_cluster(cluster, cluster_buffer);

        uint32_t cluster_offset = 0;
        if (file_pos < offset) {
            cluster_offset = offset - file_pos;
        }

        uint32_t to_copy = fs.bytes_per_cluster - cluster_offset;
        if (to_copy > size - bytes_written) {
            to_copy = size - bytes_written;
        }

        memcpy(cluster_buffer + cluster_offset, buffer + bytes_written, to_copy);
        write_cluster(cluster, cluster_buffer);

        bytes_written += to_copy;
        file_pos += fs.bytes_per_cluster;

        if (bytes_written < size) {
            uint32_t next = get_next_cluster(cluster);
            if (is_end_of_chain(next)) {
                uint32_t new_cl = alloc_cluster_zeroed();
                if (new_cl == 0) break;
                append_cluster(cluster, new_cl);
                next = new_cl;
            }
            cluster = next;
        }
    }

    // Update in-memory size if we wrote past the old end
    if (offset + bytes_written > node->size) {
        node->size = offset + bytes_written;
    }

    return (int)bytes_written;
}

// Flush file size and first cluster back to the on-disk directory entry.
// Uses private_data (parent dir cluster) to locate the entry.
int fat32_flush_size(struct vfs_node *node) {
    if (!node || !(node->flags & VFS_FILE)) return -1;

    uint32_t parent_cluster = (uint32_t)(uintptr_t)node->private_data;
    if (parent_cluster < 2) return -1;

    // Convert node name back to FAT 8.3 format for lookup
    uint8_t fat_name[11];
    string_to_fat32_name(node->name, fat_name);

    // Walk the parent directory cluster chain to find our entry
    uint32_t cluster = parent_cluster;
    while (!is_end_of_chain(cluster)) {
        read_cluster(cluster, cluster_buffer);

        int entries_per_cluster = fs.bytes_per_cluster / sizeof(struct fat32_dir_entry);
        struct fat32_dir_entry *entries = (struct fat32_dir_entry *)cluster_buffer;

        for (int i = 0; i < entries_per_cluster; i++) {
            struct fat32_dir_entry *entry = &entries[i];
            if (entry->name[0] == 0x00) return -1; // end of dir
            if (entry->name[0] == 0xE5) continue;
            if ((entry->attr & FAT32_ATTR_LFN) == FAT32_ATTR_LFN) continue;

            if (strncmp((char *)entry->name, (char *)fat_name, 11) == 0) {
                // Update size and first cluster
                entry->file_size = node->size;
                entry->first_cluster_low = node->inode & 0xFFFF;
                entry->first_cluster_high = (node->inode >> 16) & 0xFFFF;
                write_cluster(cluster, cluster_buffer);
                return 0;
            }
        }
        cluster = get_next_cluster(cluster);
    }

    return -1;
}

// Read directory entry by index
static struct dirent *fat32_readdir(struct vfs_node *node, uint32_t index) {
    if (!node || !(node->flags & VFS_DIRECTORY)) return 0;
    if (fs.bytes_per_cluster == 0 || fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) return 0;

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
                struct vfs_node *child = create_node(entry);
                if (child) {
                    child->private_data = (void *)(uintptr_t)node->inode;
                }
                return child;
            }
        }

        cluster = get_next_cluster(cluster);
    }

    return 0;
}

// ============================================================================
// Path-based helpers used by userland commands (rm, rmdir, touch, mv, ls)
// ============================================================================

int fat32_touch_path(const char *path) {
    char parent_path[VFS_MAX_PATH];
    char leaf[VFS_MAX_NAME];
    if (split_path(path, parent_path, leaf) != FAT32_E_OK) return FAT32_E_INVAL;
    if (is_special_name(leaf)) return FAT32_E_INVAL;

    struct vfs_node *parent = vfs_resolve_path(parent_path);
    if (!parent) return FAT32_E_NOENT;
    if (!(parent->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;

    if (fat32_finddir(parent, leaf)) return FAT32_E_OK; // already exists

    struct vfs_node *node = fat32_create_file(parent, leaf);
    return node ? FAT32_E_OK : FAT32_E_NOSPC;
}

int fat32_rm_path(const char *path) {
    char parent_path[VFS_MAX_PATH];
    char leaf[VFS_MAX_NAME];
    if (split_path(path, parent_path, leaf) != FAT32_E_OK) return FAT32_E_INVAL;
    struct vfs_node *parent = vfs_resolve_path(parent_path);
    if (!parent) return FAT32_E_NOENT;
    if (!(parent->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;
    return fat32_unlink(parent, leaf);
}

int fat32_rmdir_path(const char *path) {
    char parent_path[VFS_MAX_PATH];
    char leaf[VFS_MAX_NAME];
    if (split_path(path, parent_path, leaf) != FAT32_E_OK) return FAT32_E_INVAL;
    struct vfs_node *parent = vfs_resolve_path(parent_path);
    if (!parent) return FAT32_E_NOENT;
    if (!(parent->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;
    return fat32_rmdir(parent, leaf);
}

int fat32_mv_path(const char *src, const char *dst) {
    char src_parent_path[VFS_MAX_PATH], src_leaf[VFS_MAX_NAME];
    char dst_parent_path[VFS_MAX_PATH], dst_leaf[VFS_MAX_NAME];

    if (split_path(src, src_parent_path, src_leaf) != FAT32_E_OK) return FAT32_E_INVAL;
    if (split_path(dst, dst_parent_path, dst_leaf) != FAT32_E_OK) return FAT32_E_INVAL;

    if (is_special_name(src_leaf) || is_special_name(dst_leaf)) return FAT32_E_INVAL;

    struct vfs_node *src_parent = vfs_resolve_path(src_parent_path);
    struct vfs_node *dst_parent = vfs_resolve_path(dst_parent_path);
    if (!src_parent || !dst_parent) return FAT32_E_NOENT;
    if (!(src_parent->flags & VFS_DIRECTORY) || !(dst_parent->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;

    // Prevent moving a directory into its own subtree (basic check)
    int src_len = strlen(src);
    int dst_len = strlen(dst);
    if (src_len > 0 && dst_len > src_len && strncmp(dst, src, src_len) == 0 && dst[src_len] == '/') {
        return FAT32_E_INVAL;
    }

    return fat32_rename(src_parent, src_leaf, dst_parent, dst_leaf);
}

int fat32_ls_path(const char *path,
                  int (*visitor)(const struct dirent *de, void *ctx),
                  void *ctx) {
    struct vfs_node *dir = vfs_resolve_path(path);
    if (!dir) return FAT32_E_NOENT;
    if (!(dir->flags & VFS_DIRECTORY)) return FAT32_E_NOTDIR;

    uint32_t idx = 0;
    struct dirent *d;
    while ((d = fat32_readdir(dir, idx++)) != 0) {
        if (visitor) {
            if (visitor(d, ctx) != 0) break;
        }
    }
    return FAT32_E_OK;
}

int fat32_init(uint32_t partition_lba) {
    // Read boot sector
    ata_read_sectors(partition_lba, 1, sector_buffer);

    struct fat32_bpb *bpb = (struct fat32_bpb *)sector_buffer;

    // Verify it's FAT32
    if (bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0) {
        return -1;  // Not FAT32
    }
    if (bpb->bytes_per_sector != 512) {
        return -1;
    }
    if (bpb->sectors_per_cluster == 0) {
        return -1;
    }
    // Require power-of-two sectors per cluster
    if ((bpb->sectors_per_cluster & (bpb->sectors_per_cluster - 1)) != 0) {
        return -1;
    }

    // Store filesystem info
    fs.bytes_per_sector = bpb->bytes_per_sector;
    fs.sectors_per_cluster = bpb->sectors_per_cluster;
    fs.bytes_per_cluster = fs.bytes_per_sector * fs.sectors_per_cluster;
    if (fs.bytes_per_cluster > CLUSTER_BUFFER_SIZE) {
        return -1;
    }
    fs.fat_start_lba = partition_lba + bpb->reserved_sectors;
    fs.cluster_start_lba = fs.fat_start_lba + (bpb->num_fats * bpb->fat_size_32);
    fs.root_cluster = bpb->root_cluster;

    int data_sectors = bpb->total_sectors_32 - (bpb->reserved_sectors + bpb->num_fats * bpb->fat_size_32);
    fs.total_clusters = data_sectors / bpb->sectors_per_cluster;

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
