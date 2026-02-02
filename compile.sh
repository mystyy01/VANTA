#!/bin/bash
set -e

echo "=== Building VANTA OS with mt-shell ==="

# Assemble bootloader
echo "[1/8] Assembling bootloader..."
nasm -f bin bootloader/boot.asm -o boot.bin

# Assemble kernel entry and ISRs
echo "[2/8] Assembling kernel entry..."
nasm -f elf64 kernel/entry.asm -o entry.o
nasm -f elf64 kernel/isr.asm -o isr_asm.o

# Compile C kernel
echo "[3/8] Compiling kernel..."
CFLAGS="-ffreestanding -mno-red-zone -fno-pic -mcmodel=large -I kernel -I kernel/drivers -I kernel/fs"
x86_64-elf-gcc $CFLAGS -c kernel/kernel.c -o kernel.o
x86_64-elf-gcc $CFLAGS -c kernel/idt.c -o idt.o
x86_64-elf-gcc $CFLAGS -c kernel/isr.c -o isr.o
x86_64-elf-gcc $CFLAGS -c kernel/drivers/ata.c -o ata.o
x86_64-elf-gcc $CFLAGS -c kernel/drivers/keyboard.c -o keyboard.o
x86_64-elf-gcc $CFLAGS -c kernel/fs/vfs.c -o vfs.o
x86_64-elf-gcc $CFLAGS -c kernel/fs/fat32.c -o fat32.o

# Compile mt-shell lib.c (C runtime for shell)
echo "[4/8] Compiling mt-shell runtime..."
x86_64-elf-gcc $CFLAGS -I kernel -c mt-shell/lib.c -o mt-shell/lib.o

# Compile mt-shell with mt-lang compiler
echo "[5/8] Compiling mt-shell..."
cd mt-shell
mtc shell.mtc --no-runtime --obj lib.o --no-libc -o shell.o
cd ..

# Link kernel with mt-shell
echo "[6/8] Linking kernel..."
x86_64-elf-ld -T kernel/linker.ld -o kernel.bin \
    entry.o isr_asm.o kernel.o idt.o isr.o ata.o keyboard.o vfs.o fat32.o \
    mt-shell/lib.o mt-shell/shell.o

# Create boot disk image
echo "[7/8] Creating boot image..."
cat boot.bin kernel.bin > vanta.img

# Create FAT32 filesystem from testfs/
echo "[8/8] Building testfs..."
dd if=/dev/zero of=testfs.img bs=1M count=32 2>/dev/null
mkfs.fat -F 32 testfs.img >/dev/null 2>&1

# Copy files using mtools (no root required)
export MTOOLS_SKIP_CHECK=1
mcopy -i testfs.img -s testfs/* ::/ 2>/dev/null || true

echo "=== Build complete! Starting QEMU... ==="

# Run with both disks
qemu-system-x86_64 \
    -drive file=vanta.img,format=raw,index=0 \
    -drive file=testfs.img,format=raw,index=1

echo "Done"
