ENTRY(_start)
SECTIONS {
    . = 0x80010000;
    .text : { *(.text.start) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .sdata : { *(.sdata*) }
    .bss : { *(.bss*) *(COMMON) }
    _end = .;
}
