.text
.global __start

# Emit instructions exactly as written so delay slots are under our control.
.set noreorder

# Interrupt-controller self-test. It enables the VBLANK and Timer 0 interrupt
# lines, installs a handler at the general exception vector, and spins until
# both interrupts have been delivered. The handler records which I_STAT lines
# it saw. Success sets $s0 = 0xDEADBEEF.
#
#   $s1 = OR of every I_STAT value seen by the handler
#   $k0, $k1 = kernel scratch

__start:
    j    main
    nop                    # branch delay slot

# ---------------------------------------------------------------------------
# General exception handler at physical 0x80 (vector 0x80000080).
# ---------------------------------------------------------------------------
.org 0x80
handler:
    lui  $k0, 0x1f80
    lw   $k1, 0x1070($k0)  # $k1 = I_STAT (pending lines)
    nop                    # load delay slot
    or   $s1, $s1, $k1     # accumulate the lines we have seen
    sw   $k1, 0x1070($k0)  # acknowledge: I_STAT &= ~$k1 clears those lines
    mfc0 $k1, $14          # EPC (the interrupted instruction)
    nop
    jr   $k1               # a hardware interrupt resumes at EPC, not EPC+4
    rfe                    # delay slot: pop the SR interrupt/mode stack

# ---------------------------------------------------------------------------
main:
    lui  $t0, 0x1f80

    # Unmask VBLANK (bit 0) and Timer 0 (bit 4) in I_MASK (0x1F801074).
    ori  $t1, $zero, 0x11
    sw   $t1, 0x1074($t0)

    # Timer 0: target 100, reset-on-target (bit 3) + IRQ-on-target (bit 4).
    ori  $t1, $zero, 100
    sw   $t1, 0x1108($t0)   # T0 target
    ori  $t1, $zero, 0x18
    sw   $t1, 0x1104($t0)   # T0 mode (also resets the counter)

    # Enable interrupts: SR = IEc (bit 0) | IM2 (bit 10) = 0x401.
    ori  $t1, $zero, 0x401
    mtc0 $t1, $12
    nop

wait:
    andi $t2, $s1, 0x11    # have we seen both VBLANK and Timer 0?
    ori  $t3, $zero, 0x11
    bne  $t2, $t3, wait
    nop

    # Success.
    lui  $s0, 0xdead
    ori  $s0, $s0, 0xbeef
    break
