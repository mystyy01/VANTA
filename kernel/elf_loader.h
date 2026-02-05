#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include "fs/vfs.h"

// Load and execute an ELF64 binary from a VFS node.
// Returns the program's return value, or a negative error code on failure.
int elf_execute(struct vfs_node *node, char **args);
// Load an ELF64 binary into memory and return entry point via entry_out.
int elf_load(struct vfs_node *node, uint64_t *entry_out);

// Called from syscalls to return from user mode to kernel
void kernel_return_from_user(int exit_code);

#endif
