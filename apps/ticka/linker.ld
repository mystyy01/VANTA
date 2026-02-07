OUTPUT_FORMAT(elf64-x86-64)
ENTRY(main)
SECTIONS {
    . = 0x00200000;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
