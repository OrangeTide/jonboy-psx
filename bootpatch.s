.text
.set noat
.global _start

_start:
    # 1. Disable Interrupts / Reset Status Register
    # Coprocessor 0 Status Register (Reg 12) manages system interrupts.
    # Clearing it keeps the CPU in a stable kernel mode while booting.
    mtc0    $0, $12         # Status = 0 (Disable interrupts/exceptions)
    nop                     # Branch/Load delay slot

    # 2. Setup a Minimal Stack Pointer
    # The Parallel firmware might need to push registers or use local variables.
    # We point $sp near the top of the 2MB main Scratchpad/RAM region (0x801FFF00).
    lui     $29, 0x8020     # $sp = 0x80200000
    addiu   $29, $29, -256  # $sp = 0x801FFF00 (Safe, aligned stack boundary)

    # 3. Flush Cache / Context Prep
    # Optional but good practice for real PSX architecture alignment
    addiu   $2, $0, 0       # Clear return registers
    addiu   $3, $0, 0

    # 4. Jump to Parallel I/O Region
    # Uncached KSEG1 space maps Parallel IO directly to 0xBF000000, 
    # but KSEG0 Cache-Isolated entry sits at 0x9F000000. 
    lui     $4, 0x9F00      # Target base address
    jr      $4              # Jump Register to 0x9F000000
    nop                     # Branch Delay Slot (Always required on R3000A)

