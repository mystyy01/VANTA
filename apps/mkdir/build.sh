#!/bin/bash
set -e

echo "Building mkdir (C version)..."

CFLAGS="-ffreestanding -mno-red-zone -fno-pic -mcmodel=large -fno-builtin"

# Compile C source
x86_64-elf-gcc $CFLAGS -c mkdir.c -o mkdir.o

# Link into executable
x86_64-elf-ld -T linker.ld -o mkdir mkdir.o

echo "Built: mkdir"
