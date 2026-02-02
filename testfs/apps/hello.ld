OUTPUT_FORMAT(elf64-x86-64)
ENTRY(main)
SECTIONS {
    . = 0x00100000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
