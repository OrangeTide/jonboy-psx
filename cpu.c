#include "cpu.h"
#include "decode.h"
#include "mem.h"
#include <stdint.h>

R3051_CPU cpu;

void
cpu_reset(uint32_t start_pc)
{
    // Clear all architectural and delay-tracking state, then set the entry
    // point. The real reset vector is 0xBFC00000 (BIOS); the harness passes an
    // explicit address so it can run flat test binaries linked elsewhere.
    R3051_CPU fresh = { 0 };
    cpu = fresh;
    cpu.PC = start_pc;
}

void
step_cpu(void)
{
    // ---- 1. FETCH & DECODE ----
    // Fetch through the translating accessor so the virtual PC is mapped to a
    // physical address. Never index system_ram[] directly here.
    uint32_t inst = memory_read_word(cpu.PC); // Fetch
    uint32_t opcode = GET_OPCODE(inst);
    uint32_t rs     = GET_RS(inst);
    uint32_t rt     = GET_RT(inst);
    int32_t  imm    = SIGN_EXTEND_16(GET_IMM16(inst));

    // ---- 2. CAPTURE & RE-SET PENDING LOADS ----
    // We cache what needs to be written back from the LAST cycle.
    // This allows the current instruction to read the OLD register contents.
    uint32_t delayed_write_reg = cpu.load_reg;
    uint32_t delayed_write_val = cpu.load_val;

    // Reset current cycle's load slot tracking
    cpu.load_reg = 0;
    cpu.load_val = 0;

    // Branch delay initialization step...
    if (cpu.delay_slot_state == DELAY_STATE_JUMPING) {
        cpu.delay_slot_state = DELAY_STATE_DELAY_SLOT;
    }

    // ---- 2. MANAGE DELAY SLOT STATE (START OF CYCLE) ----
    // If we were flagged as "JUMPING" last cycle, we are now actively IN the delay slot.
    if (cpu.delay_slot_state == DELAY_STATE_JUMPING) {
        cpu.delay_slot_state = DELAY_STATE_DELAY_SLOT;
    }

    // ---- 3. EXECUTE CURRENT INSTRUCTION ----
    switch (opcode) {
    case 0x00: { // SPECIAL: register-register ALU ops selected by the funct field.
        // Only the halt traps are decoded here for now. The rest of the SPECIAL
        // group (ADD, OR, SLL, JR, MULT, ...) arrives in Milestone 1.
        uint32_t funct = GET_FUNCT(inst);

        switch (funct) {
        case 0x0C: // SYSCALL
        case 0x0D: // BREAK
            // Both raise exceptions on real hardware. For the test harness we
            // treat them as a clean stop so a program can signal completion.
            cpu.halted = 1;
            break;

        default:
            // Unimplemented SPECIAL function: no-op for now.
            break;
        }
    }
    break;

    case 0x04: // BEQ (Branch if Equal)

        // Crucial: Branch calculation uses the PC of the branch instruction + 4
        if (cpu.regs[rs] == cpu.regs[rt]) {
            cpu.branch_target = cpu.PC + 4 + (imm << 2);
            cpu.delay_slot_state = DELAY_STATE_JUMPING;
        }

        break;

    case 0x05: // BNE (Branch if Not Equal)
        if (cpu.regs[rs] != cpu.regs[rt]) {
            cpu.branch_target = cpu.PC + 4 + (imm << 2);
            cpu.delay_slot_state = DELAY_STATE_JUMPING;
        }

        break;

    case 0x02: // J (Jump)
        // Jump target uses top 4 bits of the current delay slot PC
        cpu.branch_target = ((cpu.PC + 4) & 0xF0000000) | (GET_TARGET(inst) << 2);
        cpu.delay_slot_state = DELAY_STATE_JUMPING;
        break;

    // ... Implement other non-branch instructions normally ...
    case 0x09: // ADDIU
        cpu.regs[rt] = cpu.regs[rs] + imm;
        break;

    case 0x10: { // COP0 (Coprocessor 0 Operations) - Opcode 010000
        // For Coprocessor instructions:
        // 'rs' acts as the sub-operation selector (MFC0 vs MTC0)
        // 'rt' is the standard CPU register source/destination
        // 'rd' is the target Coprocessor 0 system register index
        uint32_t cop_sub_op = GET_RS(inst);
        uint32_t rt         = GET_RT(inst);
        uint32_t cp0_rd     = GET_RD(inst);

        switch (cop_sub_op) {
        case 0x00: // MFC0 (Move From Coprocessor 0) - Format: mfc0 rt, rd
            if (rt != 0) {
                // Read from CP0 register 'rd' and write into CPU register 'rt'
                cpu.regs[rt] = cpu.cp0_regs[cp0_rd];
            }

            break;

        case 0x04: { // MTC0 (Move To Coprocessor 0) - Format: mtc0 rt, rd
            uint32_t val = cpu.regs[rt];

            // Write the value from CPU register 'rt' into CP0 register 'rd'
            cpu.cp0_regs[cp0_rd] = val;

            // SPECIAL BEHAVIOR: Trigger specific hardware cascades upon register changes
            if (cp0_rd == 12) { // SR (Status Register) modified
                // Games modify the SR register to enable/disable master interrupts.
                // If they enable interrupts here, we must evaluate if any are pending immediately.
                check_hardware_interrupts();
            }
        }
        break;

        case 0x10: // RFE (Restore From Exception) - Sub-funct code (bottom 6 bits == 0x10)
            if ((inst & 0x3F) == 0x10) {
                // The 'rfe' instruction pops the interrupt stack inside the Status Register (SR).
                // It restores the previous interrupt enable state bits to pull the kernel back to user mode.
                // Effectively shifts the lower 6 bits of the Status Register right by 2:
                uint32_t mode = cpu.cp0_regs[12] & 0x3F;
                cpu.cp0_regs[12] = (cpu.cp0_regs[12] & ~0x3F) | ((mode >> 2) | (mode & 0x30));

                // Re-evaluate interrupt constraints immediately after popping stack
                check_hardware_interrupts();
            }

            break;

        default:
            // printf("Unhandled Coprocessor 0 Operation: 0x%02X\n", cop_sub_op);
            break;
        }
    }
    break;

    case 0x23: { // LW (Load Word)
        uint32_t target_addr = cpu.regs[rs] + imm;

        // CRITICAL DIFFERENCE:
        // Do NOT do: cpu.regs[rt] = MEM_READ_32(target_addr);
        // Instead, queue it for the NEXT instruction slot.
        if (rt != 0) { // $0 is hardwired to 0, ignore loads to it
            cpu.load_reg = rt;
            cpu.load_val = memory_read_word(target_addr);
        }
    }
    break;

    }

    // Register $0 is hardwired to 0
    cpu.regs[0] = 0;

    // ---- 4. APPLY THE DELAYED LOAD WRITE-BACK ----
    // This happens AFTER the instruction execution is done but BEFORE the PC moves.
    if (delayed_write_reg != 0) {
        cpu.regs[delayed_write_reg] = delayed_write_val;
    }

    // ---- 5. UPDATE PROGRAM COUNTER (END OF CYCLE) ----
    if (cpu.delay_slot_state == DELAY_STATE_DELAY_SLOT) {
        // We just executed the instruction in the delay slot. Perform the scheduled jump!
        cpu.PC = cpu.branch_target;
        cpu.delay_slot_state = DELAY_STATE_NONE;
    } else {
        cpu.PC += 4;
    }
}

void
check_hardware_interrupts()
{
    uint32_t sr = cpu.cp0_regs[CP0_REG_SR];

    // Condition 1: Check if interrupts are globally enabled right now
    if ((sr & SR_IEc) == 0) {
        return; // Interrupts are disabled globally. Do nothing.
    }

    // Condition 2 & 3: Check if the PSX hardware line (IM2) is pending and allowed
    // Local PSX hardware logic must match what the MIPS status register permits.
    uint32_t local_pending = cpu.i_stat & cpu.i_mask;

    if (local_pending != 0 && (sr & SR_IM2) != 0) {
        // ---- EXCEPTION HANDLING STEP ----

        // Populate the CP0 CAUSE register (Index 13)
        // Set bit 10 to indicate hardware interrupt line 0 is firing.
        // Set bits 2-6 to 0 to indicate Exception Code 0 (INT / Hardware Interrupt).
        cpu.cp0_regs[CP0_REG_CAUSE] = (1 << 10) | (0 << 2);

        if (cpu.delay_slot_state != DELAY_STATE_NONE) {
            // Flag bit 31 of CAUSE if the exception hit inside a branch delay slot
            cpu.cp0_regs[CP0_REG_CAUSE] |= (1 << 31);
            cpu.cp0_regs[CP0_REG_EPC] = cpu.PC - 4; // Point EPC back to the branch instruction
        } else {
            cpu.cp0_regs[CP0_REG_EPC] = cpu.PC;     // Point EPC to current instruction
        }

        // --- MANAGE STATUS REGISTER STACK SHIFT ---
        // Push the interrupt state stack: Current <- Previous <- Old
        // Bits 0-5 are shifted left by 2 bits. The bottom 2 bits become 0 (Kernel mode, Ints disabled)
        uint32_t stack = sr & 0x3F;
        cpu.cp0_regs[CP0_REG_SR] = (sr & ~0x3F) | ((stack << 2) & 0x3F);

        // Vector the PC directly to the exception handler address
        // Note: Real hardware checks the SR_BEV bit, but PSX always uses 0x80000080.
        cpu.PC = 0x80000080;
        cpu.delay_slot_state = DELAY_STATE_NONE;
    }
}
