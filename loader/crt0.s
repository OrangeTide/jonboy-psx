.section .text.start, "ax"
.set noreorder
.global _start
_start:
    jal   main
    nop
_halt:
    b     _halt
    nop
