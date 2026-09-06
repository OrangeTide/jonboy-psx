#include "cpu.h"
#include "decode.h"
#include "mem.h"
#include "sched.h"
#include <stdint.h>

// Approximate CPU cost charged per instruction. The R3000 averages roughly two
// cycles per instruction; this drives the VBLANK and timer cadence.
#define CPU_CYCLES_PER_STEP 2

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
    cpu.next_pc = start_pc + 4;
    psx_hardware_reset();
}

// Schedule a branch. At execute time cpu.PC already points at the delay-slot
// instruction, so the target overwrites next_pc while the delay slot still
// runs. branch_taken flags that the following instruction is a delay slot.
static void
do_branch(uint32_t target)
{
    cpu.next_pc = target;
    cpu.branch_taken = 1;
}

// Stores are suppressed while the cache is isolated (SR bit 16). The BIOS sets
// this during cache init and expects writes to stay out of main RAM.
static int
cache_isolated(void)
{
    return (cpu.cp0_regs[CP0_REG_SR] & SR_Isc) != 0;
}

// Enter the general exception handler with the given ExcCode. Records EPC and
// the branch-delay flag, sets CAUSE, pushes the SR interrupt/mode stack, and
// vectors the PC. cpu.current_pc / cpu.in_delay_slot must already describe the
// faulting instruction.
static void
raise_exception(uint32_t exc_code)
{
    uint32_t sr = cpu.cp0_regs[CP0_REG_SR];

    // EPC points at the faulting instruction, or at the branch if the fault
    // happened in a delay slot (so the branch re-runs on return).
    if (cpu.in_delay_slot) {
        cpu.cp0_regs[CP0_REG_EPC] = cpu.current_pc - 4;
    } else {
        cpu.cp0_regs[CP0_REG_EPC] = cpu.current_pc;
    }

    // CAUSE: set ExcCode and the BD bit, and keep the interrupt-pending bits
    // (IP, bits 8-15) which mirror the hardware lines.
    uint32_t cause = (exc_code & 0x1F) << 2;

    if (cpu.in_delay_slot) {
        cause |= (1u << 31);
    }

    cause |= cpu.cp0_regs[CP0_REG_CAUSE] & 0x0000FF00;
    cpu.cp0_regs[CP0_REG_CAUSE] = cause;

    // Push the interrupt-enable / mode stack: the low 6 bits of SR shift left
    // by 2, entering kernel mode with interrupts disabled.
    uint32_t stack = sr & 0x3F;
    cpu.cp0_regs[CP0_REG_SR] = (sr & ~0x3F) | ((stack << 2) & 0x3F);

    // Vector to the general exception handler. BEV selects the ROM vector.
    uint32_t vector = (sr & SR_BEV) ? 0xBFC00180 : 0x80000080;
    cpu.PC = vector;
    cpu.next_pc = vector + 4;
    cpu.branch_taken = 0;
    cpu.in_delay_slot = 0;
}

// Update CAUSE bit 10 (IP2) from the aggregated external line, then report
// whether an interrupt is both pending and enabled. Uses the full CAUSE.IP vs
// SR.IM comparison across all eight lines.
static int
interrupt_pending(void)
{
    if (cpu.i_stat & cpu.i_mask) {
        cpu.cp0_regs[CP0_REG_CAUSE] |= (1u << 10);
    } else {
        cpu.cp0_regs[CP0_REG_CAUSE] &= ~(1u << 10);
    }

    uint32_t sr = cpu.cp0_regs[CP0_REG_SR];

    if ((sr & SR_IEc) == 0) {
        return 0; // interrupts globally disabled
    }

    return (cpu.cp0_regs[CP0_REG_CAUSE] & sr & 0x0000FF00) != 0;
}

void
step_cpu(void)
{
    // Advance hardware time first so a VBLANK or timer interrupt raised this
    // step is recognized at the boundary check just below.
    psx_hardware_tick(CPU_CYCLES_PER_STEP);

    // ---- RECOGNIZE PENDING INTERRUPTS ----
    // Interrupts are taken at instruction boundaries, before fetching the next
    // instruction, so EPC names the instruction that has not run yet.
    if (interrupt_pending()) {
        cpu.current_pc = cpu.PC;
        cpu.in_delay_slot = cpu.branch_taken; // the pending instruction is a slot
        raise_exception(EXC_INT);
        return;
    }

    // The instruction about to execute is at cpu.PC. It sits in a delay slot if
    // the previous instruction took a branch.
    cpu.current_pc = cpu.PC;
    cpu.in_delay_slot = cpu.branch_taken;

    // ---- FETCH ----
    // Address error on a misaligned instruction fetch.
    if (cpu.PC & 3) {
        cpu.cp0_regs[CP0_REG_BADVADDR] = cpu.PC;
        raise_exception(EXC_ADEL);
        return;
    }

    // Fetch through the translating accessor so the virtual PC is mapped to a
    // physical address. Never index system_ram[] directly here.
    uint32_t inst = memory_read_word(cpu.PC);

    // ---- ADVANCE PC (delay-slot aware) ----
    // The instruction just fetched runs now. cpu.PC moves to the slot a branch
    // would fill; a taken branch overwrites cpu.next_pc during execute.
    cpu.PC = cpu.next_pc;
    cpu.next_pc += 4;
    cpu.branch_taken = 0;

    // ---- CAPTURE PENDING LOAD ----
    // A load started last cycle becomes visible now, after this instruction has
    // had the chance to read the register's OLD value. Reset the slot before
    // executing so a load in THIS instruction registers cleanly.
    uint32_t delayed_reg = cpu.load_reg;
    uint32_t delayed_val = cpu.load_val;
    cpu.load_reg = 0;
    cpu.load_val = 0;

    // Snapshot the load target's current value. If the instruction about to
    // run writes that same register, the pending load is discarded (R3000
    // behavior), which we detect below by seeing the register change.
    uint32_t delayed_prev = (delayed_reg != 0) ? cpu.regs[delayed_reg] : 0;

    // ---- DECODE ----
    uint32_t opcode = GET_OPCODE(inst);
    uint32_t rs     = GET_RS(inst);
    uint32_t rt     = GET_RT(inst);
    uint32_t rd     = GET_RD(inst);
    uint32_t shamt  = GET_SHAMT(inst);
    uint32_t funct  = GET_FUNCT(inst);
    int32_t  imm_se = SIGN_EXTEND_16(GET_IMM16(inst)); // sign-extended immediate
    uint32_t imm_ze = GET_IMM16(inst);                 // zero-extended immediate

    switch (opcode) {
    case 0x00: // SPECIAL: register/register operations selected by funct
        switch (funct) {
        case 0x00: // SLL
            cpu.regs[rd] = cpu.regs[rt] << shamt;
            break;

        case 0x02: // SRL (logical)
            cpu.regs[rd] = cpu.regs[rt] >> shamt;
            break;

        case 0x03: // SRA (arithmetic)
            cpu.regs[rd] = (uint32_t)((int32_t)cpu.regs[rt] >> shamt);
            break;

        case 0x04: // SLLV
            cpu.regs[rd] = cpu.regs[rt] << (cpu.regs[rs] & 0x1F);
            break;

        case 0x06: // SRLV
            cpu.regs[rd] = cpu.regs[rt] >> (cpu.regs[rs] & 0x1F);
            break;

        case 0x07: // SRAV
            cpu.regs[rd] = (uint32_t)((int32_t)cpu.regs[rt] >> (cpu.regs[rs] & 0x1F));
            break;

        case 0x08: // JR
            do_branch(cpu.regs[rs]);
            break;

        case 0x09: { // JALR
            // Read the target before writing the link register, so a JALR with
            // rd == rs still jumps to the intended address.
            uint32_t target = cpu.regs[rs];
            cpu.regs[rd] = cpu.PC + 4; // return address = instruction after the slot
            do_branch(target);
            break;
        }

        case 0x0C: // SYSCALL
            raise_exception(EXC_SYSCALL);
            break;

        case 0x0D: // BREAK
            // BREAK is a monitor trap for the headless harness (stop and
            // report). Under a real BIOS it vectors like any other exception.
            if (cpu.halt_on_break) {
                cpu.halted = 1;
            } else {
                raise_exception(EXC_BREAK);
            }

            break;

        case 0x10: // MFHI
            cpu.regs[rd] = cpu.hi;
            break;

        case 0x11: // MTHI
            cpu.hi = cpu.regs[rs];
            break;

        case 0x12: // MFLO
            cpu.regs[rd] = cpu.lo;
            break;

        case 0x13: // MTLO
            cpu.lo = cpu.regs[rs];
            break;

        case 0x18: { // MULT (signed)
            int64_t product = (int64_t)(int32_t)cpu.regs[rs] * (int64_t)(int32_t)cpu.regs[rt];
            cpu.lo = (uint32_t)product;
            cpu.hi = (uint32_t)((uint64_t)product >> 32);
        }
        break;

        case 0x19: { // MULTU (unsigned)
            uint64_t product = (uint64_t)cpu.regs[rs] * (uint64_t)cpu.regs[rt];
            cpu.lo = (uint32_t)product;
            cpu.hi = (uint32_t)(product >> 32);
        }
        break;

        case 0x1A: { // DIV (signed) with MIPS-defined edge cases
            int32_t n = (int32_t)cpu.regs[rs];
            int32_t d = (int32_t)cpu.regs[rt];

            if (d == 0) {
                cpu.lo = (n >= 0) ? 0xFFFFFFFF : 0x00000001;
                cpu.hi = (uint32_t)n;
            } else if ((uint32_t)n == 0x80000000 && d == -1) {
                cpu.lo = 0x80000000;
                cpu.hi = 0;
            } else {
                cpu.lo = (uint32_t)(n / d);
                cpu.hi = (uint32_t)(n % d);
            }
        }
        break;

        case 0x1B: { // DIVU (unsigned)
            uint32_t n = cpu.regs[rs];
            uint32_t d = cpu.regs[rt];

            if (d == 0) {
                cpu.lo = 0xFFFFFFFF;
                cpu.hi = n;
            } else {
                cpu.lo = n / d;
                cpu.hi = n % d;
            }
        }
        break;

        case 0x20: { // ADD (traps on signed overflow)
            int32_t a = (int32_t)cpu.regs[rs];
            int32_t b = (int32_t)cpu.regs[rt];
            int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);

            if (((a ^ r) & (b ^ r)) < 0) {
                raise_exception(EXC_OVERFLOW); // destination is left unchanged
            } else {
                cpu.regs[rd] = (uint32_t)r;
            }
        }
        break;

        case 0x21: // ADDU
            cpu.regs[rd] = cpu.regs[rs] + cpu.regs[rt];
            break;

        case 0x22: { // SUB (traps on signed overflow)
            int32_t a = (int32_t)cpu.regs[rs];
            int32_t b = (int32_t)cpu.regs[rt];
            int32_t r = (int32_t)((uint32_t)a - (uint32_t)b);

            if (((a ^ b) & (a ^ r)) < 0) {
                raise_exception(EXC_OVERFLOW);
            } else {
                cpu.regs[rd] = (uint32_t)r;
            }
        }
        break;

        case 0x23: // SUBU
            cpu.regs[rd] = cpu.regs[rs] - cpu.regs[rt];
            break;

        case 0x24: // AND
            cpu.regs[rd] = cpu.regs[rs] & cpu.regs[rt];
            break;

        case 0x25: // OR
            cpu.regs[rd] = cpu.regs[rs] | cpu.regs[rt];
            break;

        case 0x26: // XOR
            cpu.regs[rd] = cpu.regs[rs] ^ cpu.regs[rt];
            break;

        case 0x27: // NOR
            cpu.regs[rd] = ~(cpu.regs[rs] | cpu.regs[rt]);
            break;

        case 0x2A: // SLT (signed)
            cpu.regs[rd] = ((int32_t)cpu.regs[rs] < (int32_t)cpu.regs[rt]) ? 1 : 0;
            break;

        case 0x2B: // SLTU (unsigned)
            cpu.regs[rd] = (cpu.regs[rs] < cpu.regs[rt]) ? 1 : 0;
            break;

        default:
            // Unimplemented SPECIAL function: no-op for now.
            break;
        }

        break;

    case 0x01: { // REGIMM: the rt field selects the branch variant
        int32_t  value = (int32_t)cpu.regs[rs];
        int      is_ge = rt & 0x01;         // 0 = less-than-zero, 1 = greater/equal-zero
        int      link  = (rt & 0x1E) == 0x10; // BLTZAL / BGEZAL also write $ra

        if (link) {
            cpu.regs[31] = cpu.PC + 4; // link happens whether or not the branch is taken
        }

        int take = is_ge ? (value >= 0) : (value < 0);

        if (take) {
            do_branch(cpu.PC + (imm_se << 2));
        }
    }
    break;

    case 0x02: // J
        do_branch((cpu.PC & 0xF0000000) | (GET_TARGET(inst) << 2));
        break;

    case 0x03: // JAL
        cpu.regs[31] = cpu.PC + 4;
        do_branch((cpu.PC & 0xF0000000) | (GET_TARGET(inst) << 2));
        break;

    case 0x04: // BEQ
        if (cpu.regs[rs] == cpu.regs[rt]) {
            do_branch(cpu.PC + (imm_se << 2));
        }

        break;

    case 0x05: // BNE
        if (cpu.regs[rs] != cpu.regs[rt]) {
            do_branch(cpu.PC + (imm_se << 2));
        }

        break;

    case 0x06: // BLEZ
        if ((int32_t)cpu.regs[rs] <= 0) {
            do_branch(cpu.PC + (imm_se << 2));
        }

        break;

    case 0x07: // BGTZ
        if ((int32_t)cpu.regs[rs] > 0) {
            do_branch(cpu.PC + (imm_se << 2));
        }

        break;

    case 0x08: { // ADDI (traps on signed overflow)
        int32_t a = (int32_t)cpu.regs[rs];
        int32_t r = (int32_t)((uint32_t)a + (uint32_t)imm_se);

        if (((a ^ r) & (imm_se ^ r)) < 0) {
            raise_exception(EXC_OVERFLOW);
        } else {
            cpu.regs[rt] = (uint32_t)r;
        }
    }
    break;

    case 0x09: // ADDIU (no overflow trap)
        cpu.regs[rt] = cpu.regs[rs] + (uint32_t)imm_se;
        break;

    case 0x0A: // SLTI (signed)
        cpu.regs[rt] = ((int32_t)cpu.regs[rs] < imm_se) ? 1 : 0;
        break;

    case 0x0B: // SLTIU (unsigned compare against sign-extended immediate)
        cpu.regs[rt] = (cpu.regs[rs] < (uint32_t)imm_se) ? 1 : 0;
        break;

    case 0x0C: // ANDI
        cpu.regs[rt] = cpu.regs[rs] & imm_ze;
        break;

    case 0x0D: // ORI
        cpu.regs[rt] = cpu.regs[rs] | imm_ze;
        break;

    case 0x0E: // XORI
        cpu.regs[rt] = cpu.regs[rs] ^ imm_ze;
        break;

    case 0x0F: // LUI
        cpu.regs[rt] = imm_ze << 16;
        break;

    case 0x10: { // COP0 (Coprocessor 0 Operations)
        uint32_t cop_sub_op = rs;

        switch (cop_sub_op) {
        case 0x00: // MFC0 (Move From Coprocessor 0): mfc0 rt, rd
            if (rt != 0) {
                cpu.regs[rt] = cpu.cp0_regs[rd];
            }

            break;

        case 0x04: // MTC0 (Move To Coprocessor 0): mtc0 rt, rd
            cpu.cp0_regs[rd] = cpu.regs[rt];
            // Enabling interrupts through SR takes effect at the next
            // instruction boundary, where step_cpu re-checks for pending lines.
            break;

        case 0x10: // RFE (Restore From Exception)
            if ((inst & 0x3F) == 0x10) {
                // Pop the interrupt-enable / mode stack: shift the low 6 bits of
                // SR right by 2, keeping the top pair (old) in place.
                uint32_t mode = cpu.cp0_regs[12] & 0x3F;
                cpu.cp0_regs[12] = (cpu.cp0_regs[12] & ~0x3F) | ((mode >> 2) | (mode & 0x30));
            }

            break;

        default:
            break;
        }
    }
    break;

    // ---- LOADS (result appears after the following instruction) ----
    case 0x20: // LB (sign-extended byte)
        if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = (uint32_t)(int32_t)(int8_t)memory_read_byte(cpu.regs[rs] + imm_se);
        }

        break;

    case 0x21: { // LH (sign-extended halfword)
        uint32_t addr = cpu.regs[rs] + imm_se;

        if (addr & 1) {
            cpu.cp0_regs[CP0_REG_BADVADDR] = addr;
            raise_exception(EXC_ADEL);
        } else if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = (uint32_t)(int32_t)(int16_t)memory_read_halfword(addr);
        }
    }
    break;

    case 0x23: { // LW
        uint32_t addr = cpu.regs[rs] + imm_se;

        if (addr & 3) {
            cpu.cp0_regs[CP0_REG_BADVADDR] = addr;
            raise_exception(EXC_ADEL);
        } else if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = memory_read_word(addr);
        }
    }
    break;

    case 0x24: // LBU (zero-extended byte)
        if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = memory_read_byte(cpu.regs[rs] + imm_se);
        }

        break;

    case 0x25: { // LHU (zero-extended halfword)
        uint32_t addr = cpu.regs[rs] + imm_se;

        if (addr & 1) {
            cpu.cp0_regs[CP0_REG_BADVADDR] = addr;
            raise_exception(EXC_ADEL);
        } else if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = memory_read_halfword(addr);
        }
    }
    break;

    case 0x22: { // LWL (load word left, unaligned)
        uint32_t addr    = cpu.regs[rs] + imm_se;
        uint32_t aligned = memory_read_word(addr & ~3u);
        // An LWL/LWR pair targets the same register back to back; take the
        // in-flight value if a load to rt is still pending.
        uint32_t cur = (cpu.load_reg == rt) ? cpu.load_val : cpu.regs[rt];
        uint32_t val;

        switch (addr & 3) {
        case 0:  val = (cur & 0x00FFFFFF) | (aligned << 24); break;
        case 1:  val = (cur & 0x0000FFFF) | (aligned << 16); break;
        case 2:  val = (cur & 0x000000FF) | (aligned << 8);  break;
        default: val = aligned; break;
        }

        if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = val;
        }
    }
    break;

    case 0x26: { // LWR (load word right, unaligned)
        uint32_t addr    = cpu.regs[rs] + imm_se;
        uint32_t aligned = memory_read_word(addr & ~3u);
        uint32_t cur = (cpu.load_reg == rt) ? cpu.load_val : cpu.regs[rt];
        uint32_t val;

        switch (addr & 3) {
        case 0:  val = aligned; break;
        case 1:  val = (cur & 0xFF000000) | (aligned >> 8);  break;
        case 2:  val = (cur & 0xFFFF0000) | (aligned >> 16); break;
        default: val = (cur & 0xFFFFFF00) | (aligned >> 24); break;
        }

        if (rt != 0) {
            cpu.load_reg = rt;
            cpu.load_val = val;
        }
    }
    break;

    // ---- STORES ----
    case 0x28: // SB
        if (!cache_isolated()) {
            memory_write_byte(cpu.regs[rs] + imm_se, cpu.regs[rt] & 0xFF);
        }

        break;

    case 0x29: { // SH
        uint32_t addr = cpu.regs[rs] + imm_se;

        if (addr & 1) {
            cpu.cp0_regs[CP0_REG_BADVADDR] = addr;
            raise_exception(EXC_ADES);
        } else if (!cache_isolated()) {
            memory_write_halfword(addr, cpu.regs[rt] & 0xFFFF);
        }
    }
    break;

    case 0x2B: { // SW
        uint32_t addr = cpu.regs[rs] + imm_se;

        if (addr & 3) {
            cpu.cp0_regs[CP0_REG_BADVADDR] = addr;
            raise_exception(EXC_ADES);
        } else if (!cache_isolated()) {
            memory_write_word(addr, cpu.regs[rt]);
        }
    }
    break;

    case 0x2A: { // SWL (store word left, unaligned)
        uint32_t addr    = cpu.regs[rs] + imm_se;
        uint32_t aligned = addr & ~3u;
        uint32_t mem     = memory_read_word(aligned);
        uint32_t reg     = cpu.regs[rt];
        uint32_t val;

        switch (addr & 3) {
        case 0:  val = (mem & 0xFFFFFF00) | (reg >> 24); break;
        case 1:  val = (mem & 0xFFFF0000) | (reg >> 16); break;
        case 2:  val = (mem & 0xFF000000) | (reg >> 8);  break;
        default: val = reg; break;
        }

        if (!cache_isolated()) {
            memory_write_word(aligned, val);
        }
    }
    break;

    case 0x2E: { // SWR (store word right, unaligned)
        uint32_t addr    = cpu.regs[rs] + imm_se;
        uint32_t aligned = addr & ~3u;
        uint32_t mem     = memory_read_word(aligned);
        uint32_t reg     = cpu.regs[rt];
        uint32_t val;

        switch (addr & 3) {
        case 0:  val = reg; break;
        case 1:  val = (mem & 0x000000FF) | (reg << 8);  break;
        case 2:  val = (mem & 0x0000FFFF) | (reg << 16); break;
        default: val = (mem & 0x00FFFFFF) | (reg << 24); break;
        }

        if (!cache_isolated()) {
            memory_write_word(aligned, val);
        }
    }
    break;

    default:
        // Unimplemented primary opcode: no-op for now.
        break;
    }

    // Register $0 is hardwired to 0 (guard against writes above).
    cpu.regs[0] = 0;

    // ---- APPLY THE DELAYED LOAD WRITE-BACK ----
    // Applied after execution so the instruction in the load-delay slot read
    // the register's previous value. If that instruction wrote the load
    // target register itself, its write wins and the load is discarded, so
    // only apply the loaded value when the register was left untouched.
    if (delayed_reg != 0 && cpu.regs[delayed_reg] == delayed_prev) {
        cpu.regs[delayed_reg] = delayed_val;
    }

    cpu.regs[0] = 0;
}
