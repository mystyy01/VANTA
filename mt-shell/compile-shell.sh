#!/bin/bash
set -e

# Compile bare-metal C library
x86_64-elf-gcc -c lib.c -o lib.o -ffreestanding -mno-red-zone -fno-pic -mcmodel=large -O2

# Compile mt-lang tokenizer with no libc, linking lib.o
mtc tokenizer.mtc --no-libc --obj lib.o out
