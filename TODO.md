# PHOBOS TODO

## Pipes & IPC (Step 1 - In Progress)

- [x] Per-process file descriptor table
- [x] Per-process current working directory
- [x] `pipe()` syscall
- [x] Pipe read/write support
- [x] `dup2()` syscall
- [x] Pipe close handling
- [x] Test program (pipestest)
- [ ] Shell pipe parsing (`cmd1 | cmd2`)

## Filesystem

- [ ] **FAT32 Long File Name (LFN) support** — currently limited to 8.3 names
  - Detect LFN entries (attribute 0x0F)
  - Reassemble long names from multiple entries
  - Generate LFN entries on file creation

- [ ] **Consider ext4 migration** — benefits:
  - Native long filename support
  - Better for Unix-like OS (permissions, symlinks, etc.)
  - More modern design
  - Cons: More complex to implement from scratch

## Process Model (Step 2 - Not Started)

- [ ] `fork()` syscall
- [ ] `exec()` syscall
- [ ] Process groups and sessions
- [ ] Signals (SIGINT, SIGTERM, etc.)
- [ ] Job control (bg, fg, jobs, &)

## Notes

- ext4 is significantly more complex than FAT32 but worth it long-term
- Alternative: keep FAT32 for boot/simple stuff, add ext4 as primary filesystem
- Shell pipe parsing requires either fork() or modifying how shell spawns programs
