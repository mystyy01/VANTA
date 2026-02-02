# Repository Guidelines

## Project Structure & Module Organization
- `bootloader/` contains the 512‑byte bootloader (`boot.asm`).
- `kernel/` holds kernel sources in C and ASM, plus the linker script (`linker.ld`).
  - `kernel/drivers/` includes low‑level device drivers (ATA, keyboard).
  - `kernel/fs/` includes the VFS layer and FAT32 implementation.
- `mt-shell/` stores mt‑lang modules (e.g., `tokenizer.mtc`).
- `testfs/` is copied into `testfs.img` at build time (FAT32 payload).
- Build artifacts land in the repo root (`*.o`, `*.bin`, `*.img`).

## Build, Test, and Development Commands
- `./compile.sh` builds the bootloader + kernel, creates disk images, and launches QEMU.
  - Produces `vanta.img` (boot disk) and `testfs.img` (FAT32 test disk).
  - Requires: `nasm`, `x86_64-elf-gcc`, `x86_64-elf-ld`, `qemu-system-x86_64`, `mtools`, `mkfs.fat`.
- No dedicated clean script; remove artifacts manually if needed (e.g., `rm -f *.o *.bin *.img`).

## Coding Style & Naming Conventions
- C follows 4‑space indentation with K&R braces and `snake_case` functions.
- Headers live alongside sources (`idt.c` + `idt.h`); include with relative paths (e.g., `#include "drivers/ata.h"`).
- Keep code freestanding: avoid standard library calls and dynamic allocation unless explicitly implemented.

## Testing Guidelines
- Automated tests are not present. Validate changes by booting in QEMU via `./compile.sh`.
- For filesystem work, add fixtures to `testfs/` and confirm they are readable after boot.
- When touching interrupts or drivers, verify boot output and basic keyboard input in the QEMU console.

## Commit & Pull Request Guidelines
- Existing history uses short, descriptive commit subjects without prefixes (e.g., “basic bootloader”).
- Match that style: concise present‑tense summaries, one line per change group.
- PRs should include: purpose, build command used, and any runtime evidence (boot log or screenshot) when behavior changes.
