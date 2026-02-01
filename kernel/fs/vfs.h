#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_FILE      0x01
#define VFS_DIRECTORY 0x02
#define VFS_MAX_PATH  256
#define VFS_MAX_NAME  128

// Forward declarations
struct vfs_node;
struct dirent;

// Function pointer types for filesystem operations
typedef int (*read_fn)(struct vfs_node *, uint32_t offset, uint32_t size, uint8_t *buffer);
typedef int (*write_fn)(struct vfs_node *, uint32_t offset, uint32_t size, const uint8_t *buffer);
typedef struct dirent *(*readdir_fn)(struct vfs_node *, uint32_t index);
typedef struct vfs_node *(*finddir_fn)(struct vfs_node *, const char *name);

// Filesystem node (file or directory)
struct vfs_node {
    char name[VFS_MAX_NAME];
    uint32_t flags;       // VFS_FILE or VFS_DIRECTORY
    uint32_t size;
    uint32_t inode;       // Filesystem-specific identifier

    // Operations
    read_fn read;
    write_fn write;
    readdir_fn readdir;
    finddir_fn finddir;

    // Filesystem-specific data
    void *private_data;
};

// Directory entry
struct dirent {
    char name[VFS_MAX_NAME];
    uint32_t inode;
};

// VFS operations
struct vfs_node *vfs_root(void);
void vfs_set_root(struct vfs_node *node);

int vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer);
int vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer);
struct dirent *vfs_readdir(struct vfs_node *node, uint32_t index);
struct vfs_node *vfs_finddir(struct vfs_node *node, const char *name);

// Path resolution
struct vfs_node *vfs_resolve_path(const char *path);

#endif
