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
        *(.bss)
        *(.lbss)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.note.GNU-stack)
    }
}
