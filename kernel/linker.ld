OUTPUT_FORMAT(binary)
ENTRY(_start)

SECTIONS
{
    . = 0x7E00;

    .text : {
        *(.text)
        *(.ltext)
    }

    .rodata : {
        *(.rodata)
        *(.rodata.*)
        *(.lrodata)
    }

    .data : {
        *(.data)
        *(.ldata)
    }

    .bss : {
        __bss_start = .;
        *(.bss)
        *(.lbss)
        *(COMMON)
        __bss_end = .;
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}
