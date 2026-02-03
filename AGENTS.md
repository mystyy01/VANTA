# PHOBOS Development Guidelines

## Project Architecture
- **Real-mode bootloader**: `bootloader/boot.asm` (512 bytes, loads kernel at 0x7E00)
- **64-bit kernel**: C + Assembly modules in `kernel/`
  - Core: `kernel.c`, `entry.asm`, `isr.asm`, `idt.c` 
  - Drivers: `kernel/drivers/ata.c`, `kernel/drivers/keyboard.c`
  - Filesystem: `kernel/fs/vfs.c`, `kernel/fs/fat32.c`
- **Shell**: `mt-shell/` contains mt-lang runtime (`lib.c`) and shell source (`shell.mtc`)
- **Test payload**: `testfs/` directory contents are copied to `testfs.img` (FAT32)
- **Linker script**: `kernel/linker.ld` sets kernel load address to 0x7E00

## Build System & Commands
### Primary Build
```bash
./compile.sh    # Full build: compile, link, create images, launch QEMU
```

### Build Requirements
- `nasm` (assembly)
- `x86_64-elf-gcc`, `x86_64-elf-ld` (cross-compilation)
- `qemu-system-x86_64` (emulation)
- `mtools`, `mkfs.fat` (FAT32 image creation)

### Manual Build Steps
```bash
# Assemble bootloader
nasm -f bin bootloader/boot.asm -o boot.bin

# Assemble kernel components
nasm -f elf64 kernel/entry.asm -o entry.o
nasm -f elf64 kernel/isr.asm -o isr_asm.o

# Compile C kernel modules
CFLAGS="-ffreestanding -mno-red-zone -fno-pic -mcmodel=large -I kernel -I kernel/drivers -I kernel/fs"
x86_64-elf-gcc $CFLAGS -c kernel/kernel.c -o kernel.o
x86_64-elf-gcc $CFLAGS -c kernel/idt.c -o idt.o
x86_64-elf-gcc $CFLAGS -c kernel/isr.c -o isr.o
x86_64-elf-gcc $CFLAGS -c kernel/drivers/ata.c -o ata.o
x86_64-elf-gcc $CFLAGS -c kernel/drivers/keyboard.c -o keyboard.o
x86_64-elf-gcc $CFLAGS -c kernel/fs/vfs.c -o vfs.o
x86_64-elf-gcc $CFLAGS -c kernel/fs/fat32.c -o fat32.o

# Link kernel
x86_64-elf-ld -T kernel/linker.ld -o kernel.bin entry.o isr_asm.o kernel.o idt.o isr.o ata.o keyboard.o vfs.o fat32.o

# Create boot disk
cat boot.bin kernel.bin > phobos.img
```

### Testing & Validation
- **Boot test**: `./compile.sh` - verify kernel loads and prints to VGA memory (0xB8000)
- **Filesystem test**: Add files to `testfs/`, confirm FAT32 mount shows green "FAT32 mounted" 
- **Driver test**: Check keyboard input responds, ATA communication succeeds
- **No unit tests**: Manual QEMU testing required for all changes

## Code Style & Conventions

### C Language Standards
- **Freestanding environment**: No standard library, system calls, or dynamic allocation
- **Compiler flags**: `-ffreestanding -mno-red-zone -fno-pic -mcmodel=large`
- **Headers**: Include guards with `#ifndef HEADER_H #define HEADER_H #endif`
- **Types**: Use `<stdint.h>` types (`uint32_t`, `uint8_t`, etc.)
- **Structures**: Use `__attribute__((packed))` for hardware registers

### Formatting & Naming
- **Indentation**: 4 spaces, no tabs
- **Brace style**: K&R (opening brace on same line)
- **Function names**: `snake_case`
- **Variable names**: `snake_case` for locals, `snake_case` for globals
- **Constants**: `UPPER_SNAKE_CASE` with `#define`
- **File organization**: `.c` and `.h` pairs in same directory

### Hardware & Low-Level Code
- **VGA output**: Write to `(volatile unsigned short *)0xB8000` (char + color)
- **Ports**: Use inline assembly or proper I/O functions for hardware access
- **Interrupts**: IDT entries with proper selectors and type attributes
- **Memory**: Static buffers only, no heap allocation
- **Register access**: Volatile pointers for memory-mapped hardware

### Error Handling Patterns
- **Return codes**: Use `int` return values (0 = success, negative = error)
- **Status checks**: Test hardware status bits before operations
- **Fallback paths**: Provide graceful degradation (e.g., "FAT32 failed" in red)
- **No exceptions**: C code cannot use exception handling

### Filesystem Implementation
- **VFS abstraction**: Generic `vfs_node` structures with function pointers
- **FAT32 specifics**: Cluster-to-LBA conversion, directory entry parsing
- **Buffer management**: Fixed-size sector/cluster buffers (512B/4KB)
- **String operations**: Implement `strlen`, `memcpy`, `memset`, `strncmp` locally

### Driver Development
- **ATA**: Primary bus ports (0x1F0-0x1F7), handle BSY/DRDY/DRQ status bits
- **Keyboard**: PS/2 controller initialization and interrupt handling
- **Interrupt handling**: ISR assembly stubs calling C handlers

## Development Workflow

### Making Changes
1. **Modify source files** following existing patterns
2. **Build with `./compile.sh`** to catch compilation errors
3. **Test in QEMU**: Verify boot messages, driver initialization, filesystem mounting
4. **Clean up**: Remove artifacts with `rm -f *.o *.bin *.img` if needed

### Common Validation Points
- **Kernel boot**: Should print "PHOBOS - 64-bit C Kernel" at top of screen
- **Keyboard**: Type input should work after shell starts
- **ATA**: Should not hang on disk access
- **FAT32**: Should show "FAT32 mounted" in green when successful
- **Shell**: mt-shell prompt should appear and accept commands

### Commit Style
- **Subject line**: Short, descriptive, present tense (e.g., "add ATA driver support")
- **No prefixes**: Avoid tags like "feat:" or "fix:"
- **One change per commit**: Group related file changes together
- **PR descriptions**: Include what changed, why, and how to test

## Architecture Notes
- **Memory layout**: Kernel loads at 0x7E00, video memory at 0xB8000
- **Interrupt system**: IDT setup required for keyboard and hardware interrupts  
- **Filesystem stack**: VFS → FAT32 → ATA → disk
- **Shell integration**: mt-shell runs as userland process within kernel space
