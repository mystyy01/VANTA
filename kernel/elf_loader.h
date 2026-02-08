#ifndef ELF_LOADER_H
#define ELF_LOADER_H

#include <stdint.h>
#include "fs/vfs.h"

// Load and execute an ELF64 binary from a VFS node (legacy blocking path).
// Returns the program's return value, or a negative error code on failure.
int elf_execute(struct vfs_node *node, char **args);

// Load an ELF64 binary into the identity-mapped address space (legacy).
int elf_load(struct vfs_node *node, uint64_t *entry_out);

// Load an ELF64 binary into a per-process page table.
// Allocates fresh physical pages for each segment and maps them at p_vaddr.
// Returns 0 on success, <0 on error.  *entry_out receives the entry point VA.
int elf_load_into(struct vfs_node *node, uint64_t *user_pml4, uint64_t *entry_out);

// Called from syscalls to return from user mode to kernel
void kernel_return_from_user(int exit_code);

#endif
