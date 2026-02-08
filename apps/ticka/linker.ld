OUTPUT_FORMAT(elf64-x86-64)
ENTRY(main)
SECTIONS {
    . = 0x01000000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
