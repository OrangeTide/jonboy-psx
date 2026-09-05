.text
.global __start

# Emit instructions exactly as written so delay slots are under our control.
.set noreorder

# Exception self-test. It installs a handler at the general exception vector
# (0x80000080, physical 0x80), deliberately triggers three synchronous
# exceptions, and checks that each vectored correctly, left its destination
# register untouched, and returned to the instruction after the fault.
#
# The handler accumulates state in callee-saved registers:
#   $s1 = number of exceptions taken
#   $s2 = ExcCode of the most recent exception
#   $s3 = bitmask of (1 << ExcCode) for every exception seen
#
# Success sets $s0 = 0xDEADBEEF; any failure sets a smaller nonzero code.

__start:
    j    main
    nop                    # branch delay slot

# ---------------------------------------------------------------------------
# General exception handler, placed at physical 0x80 to match vector
# 0x80000080 (BEV = 0). $k0 and $k1 are kernel scratch registers.
# ---------------------------------------------------------------------------
.org 0x80
exc_handler:
    mfc0 $k0, $13          # CAUSE
    srl  $k0, $k0, 2
    andi $k0, $k0, 0x1f    # ExcCode field
    addiu $s1, $s1, 1      # count this exception
    or   $s2, $zero, $k0   # remember the most recent code
    ori  $k1, $zero, 1
    sllv $k1, $k1, $k0     # 1 << ExcCode
    or   $s3, $s3, $k1     # OR it into the seen-mask
    mfc0 $k1, $14          # EPC
    addiu $k1, $k1, 4      # resume at the instruction after the faulting one
    jr   $k1
    rfe                    # delay slot: pop the SR interrupt/mode stack

# ---------------------------------------------------------------------------
main:
    # --- Test A: arithmetic overflow (EXC_OVERFLOW = 0x0C) ---
    ori  $t2, $zero, 0     # destination starts at 0
    lui  $t0, 0x7fff
    ori  $t0, $t0, 0xffff  # $t0 = 0x7FFFFFFF
    ori  $t1, $zero, 1
    add  $t2, $t0, $t1     # overflow: must trap and leave $t2 == 0

    # --- Test B: syscall (EXC_SYSCALL = 0x08) ---
    syscall

    # --- Test C: address error on an unaligned load (EXC_ADEL = 0x04) ---
    ori  $t4, $zero, 0     # destination starts at 0
    ori  $t3, $zero, 0x1001 # misaligned word address
    lw   $t4, 0($t3)       # must trap and leave $t4 == 0

    # --- Verify results ---
    ori  $t9, $zero, 3
    bne  $s1, $t9, fail    # exactly three exceptions must have fired
    nop
    ori  $t9, $zero, 0x1110 # (1<<12) | (1<<8) | (1<<4)
    bne  $s3, $t9, fail    # overflow, syscall, and AdEL must all be seen
    nop
    bne  $t2, $zero, fail  # overflow must not have written its destination
    nop
    bne  $t4, $zero, fail  # the bad load must not have written its destination
    nop

    # --- Success ---
    lui  $s0, 0xdead
    ori  $s0, $s0, 0xbeef
    break

fail:
    ori  $s0, $zero, 0xbad # nonzero and not 0xDEADBEEF
    break
