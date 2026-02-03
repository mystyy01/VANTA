#include "vfs.h"

static struct vfs_node *root_node = 0;

struct vfs_node *vfs_root(void) {
    return root_node;
}

void vfs_set_root(struct vfs_node *node) {
    root_node = node;
}

int vfs_read(struct vfs_node *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node && node->read) {
        return node->read(node, offset, size, buffer);
    }
    return -1;
}

int vfs_write(struct vfs_node *node, uint32_t offset, uint32_t size, const uint8_t *buffer) {
    if (node && node->write) {
        return node->write(node, offset, size, buffer);
    }
    return -1;
}

struct dirent *vfs_readdir(struct vfs_node *node, uint32_t index) {
    if (node && (node->flags & VFS_DIRECTORY) && node->readdir) {
        return node->readdir(node, index);
    }
    return 0;
}

struct vfs_node *vfs_finddir(struct vfs_node *node, const char *name) {
    if (node && (node->flags & VFS_DIRECTORY) && node->finddir) {
        return node->finddir(node, name);
    }
    return 0;
}

// Simple string compare
static int strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a - *b;
}

// Copy string
static void strcpy(char *dest, const char *src) {
    while (*src) {
        *dest++ = *src++;
    }
    *dest = 0;
}

struct vfs_node *vfs_resolve_path(const char *path) {
    if (!path || !root_node) return 0;

    // Handle root
    if (path[0] == '/' && path[1] == 0) {
        return root_node;
    }

    struct vfs_node *current = root_node;
    struct vfs_node *stack[VFS_MAX_PATH / 2];
    int depth = 0;
    char component[VFS_MAX_NAME];
    int i = 0;

    // Skip leading slash
    if (*path == '/') path++;

    while (*path) {
        // Extract path component
        i = 0;
        while (*path && *path != '/') {
            if (i < VFS_MAX_NAME - 1) {
                component[i++] = *path;
            }
            path++;
        }
        component[i] = 0;

        // Skip trailing slash
        if (*path == '/') path++;

        // Find this component in current directory
        if (component[0]) {
            if (strcmp(component, ".") == 0) {
                continue;
            }
            if (strcmp(component, "..") == 0) {
                if (depth > 0) {
                    current = stack[--depth];
                } else {
                    current = root_node;
                }
                continue;
            }

            if (depth < (VFS_MAX_PATH / 2)) {
                stack[depth++] = current;
            } else {
                return 0;
            }

            current = vfs_finddir(current, component);
            if (!current) return 0;
        }
    }

    return current;
}
