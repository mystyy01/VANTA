#!/bin/bash
set -e

# Assemble bootloader
nasm -f bin bootloader/boot.asm -o boot.bin

# Assemble kernel entry and ISRs
nasm -f elf64 kernel/entry.asm -o entry.o
nasm -f elf64 kernel/isr.asm -o isr_asm.o

# Compile C kernel
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

# Create boot disk image
cat boot.bin kernel.bin > vanta.img

# Create FAT32 filesystem from testfs/
echo "Building testfs..."
dd if=/dev/zero of=testfs.img bs=1M count=32 2>/dev/null
mkfs.fat -F 32 testfs.img >/dev/null 2>&1

# Copy files using mtools (no root required)
export MTOOLS_SKIP_CHECK=1
mcopy -i testfs.img -s testfs/* ::/ 2>/dev/null || true

# Run with both disks
qemu-system-x86_64 \
    -drive file=vanta.img,format=raw,index=0 \
    -drive file=testfs.img,format=raw,index=1

echo "Done"
