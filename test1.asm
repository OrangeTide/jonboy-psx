.text
.global __start

__start:
    # -------------------------------------------------------------------------
    # TEST 1: Basic Register Operations & Immediate Arithmetic (ADDIU, ORI)
    # -------------------------------------------------------------------------
    li   $s0, 1         # Set failure code to 1 (Test 1 Failed)
    ori  $t0, $zero, 0x5555
    addiu $t1, $t0, 0x1111
    li   $t2, 0x6666
    bne  $t1, $t2, fail # If 0x5555 + 0x1111 != 0x6666, fail.
    nop                 # Branch delay slot

    # -------------------------------------------------------------------------
    # TEST 2: Sign Extension (ADDIU with negative values)
    # -------------------------------------------------------------------------
    li   $s0, 2         # Set failure code to 2 (Test 2 Failed)
    addiu $t0, $zero, -5 # Should sign-extend to 0xFFFFFFFB
    li   $t1, 0xFFFFFFFB
    bne  $t0, $t1, fail
    nop                 # Branch delay slot

    # -------------------------------------------------------------------------
    # TEST 3: Logical Bitwise Operations (AND, OR, XOR)
    # -------------------------------------------------------------------------
    li   $s0, 3         # Set failure code to 3 (Test 3 Failed)
    li   $t0, 0x0F0F0F0F
    li   $t1, 0xF0F0F0F0
    or   $t2, $t0, $t1  # Expected: 0xFFFFFFFF
    not  $t3, $zero     # Max value
    bne  $t2, $t3, fail
    nop                 # Branch delay slot

    and  $t2, $t0, $t1  # Expected: 0x00000000
    bne  $t2, $zero, fail
    nop                 # Branch delay slot

    # -------------------------------------------------------------------------
    # TEST 4: Shift Operations (SLL, SRL, SRA)
    # -------------------------------------------------------------------------
    li   $s0, 4         # Set failure code to 4 (Test 4 Failed)
    li   $t0, 0x00000001
    sll  $t1, $t0, 4    # 1 << 4 = 16 (0x10)
    li   $t2, 0x10
    bne  $t1, $t2, fail
    nop                 # Branch delay slot

    li   $t0, 0x80000000 # Negative sign bit set
    sra  $t1, $t0, 1    # Arithmetic shift right should preserve sign
    li   $t2, 0xC0000000
    bne  $t1, $t2, fail
    nop                 # Branch delay slot

    # -------------------------------------------------------------------------
    # TEST 5: Branch Delay Slot Behavior
    # -------------------------------------------------------------------------
    li   $s0, 5         # Set failure code to 5 (Test 5 Failed)
    li   $t0, 0
    beq  $zero, $zero, target_branch
    li   $t0, 42        # DELAY SLOT: This MUST execute before branching!

target_branch:
    li   $t1, 42
    bne  $t0, $t1, fail # If $t0 isn't 42, your delay slot didn't execute
    nop                 # Branch delay slot

    # -------------------------------------------------------------------------
    # TEST 6: MIPS-I Load Delay Slot Verification
    # -------------------------------------------------------------------------
    li   $s0, 6         # Set failure code to 6 (Test 6 Failed)

    # Setup a safe scratchpad address in RAM using a temporary pointer.
    # Assumes address 0x00001000 is writeable mirror/RAM space.
    li   $t0, 0x00001000
    li   $t1, 0xABCDEFFF
    sw   $t1, 0($t0)    # Store it to memory

    # The Core Load Delay Test:
    lw   $t2, 0($t0)    # Load value into $t2
    add  $t3, $t2, $zero # CRITICAL: This instruction executes BEFORE $t2 updates!
                        # Therefore, $t3 should get the OLD value of $t2 (0).

    bne  $t3, $zero, fail # If $t3 is NOT zero, your emulator updated $t2 too fast!
    nop

    # Verify the load finished on the *subsequent* instruction
    li   $t1, 0xABCDEFFF
    bne  $t2, $t1, fail # If $t2 isn't updated by now, loads are broken entirely.
    nop

    # -------------------------------------------------------------------------
    # SUCCESS TRIGGER
    # -------------------------------------------------------------------------
    li   $s0, 0xDEADBEEF # Magic constant indicating total success!
    break               # Tell emulator we are done.

fail:
    break               # Emulator halts here. Check $s0 to see which test failed.
