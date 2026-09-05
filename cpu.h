#ifndef CPU_H_
#define CPU_H_
#include <stdint.h>

// ============================================================================
// MIPS-I STATUS REGISTER (SR / CP0 REG 12) BITMASKS
// ============================================================================

// 1. Interrupt Enable Stack (Bits 0-5)
// The R3051 maintains a 3-level deep kernel/user and interrupt enable stack.
// When an exception occurs, these shift left. When 'RFE' executes, they shift right.
#define SR_IEc      (1 << 0)  // Interrupt Enable (Current): 1 = Enabled, 0 = Disabled
#define SR_KUc      (1 << 1)  // Kernel/User Mode (Current):  0 = Kernel,  1 = User
#define SR_IEp      (1 << 2)  // Interrupt Enable (Previous)
#define SR_KUp      (1 << 3)  // Kernel/User Mode (Previous)
#define SR_IEo      (1 << 4)  // Interrupt Enable (Old)
#define SR_KUo      (1 << 5)  // Kernel/User Mode (Old)

// 2. Interrupt Mask (Bits 8-15)
// Controls which individual interrupt lines are allowed to disrupt the CPU.
// Bits 8-9 are software interrupts. Bits 10-15 are hardware interrupts.
#define SR_IM0      (1 << 8)  // Software Interrupt Mask 0
#define SR_IM1      (1 << 9)  // Software Interrupt Mask 1
#define SR_IM2      (1 << 10) // Hardware Interrupt Mask 0 (PSX Master Peripheral Line)
#define SR_IM3      (1 << 11) // Hardware Interrupt Mask 1
#define SR_IM4      (1 << 12) // Hardware Interrupt Mask 2
#define SR_IM5      (1 << 13) // Hardware Interrupt Mask 3
#define SR_IM6      (1 << 14) // Hardware Interrupt Mask 4
#define SR_IM7      (1 << 15) // Hardware Interrupt Mask 5 (Internal CPU Timer)
#define SR_IM_MASK  (0xFF << 8) // Full combined 8-bit Interrupt Mask

// 3. Diagnostic & Status Status Flags (Bits 16-22)
#define SR_Isc      (1 << 16) // IsC: Isolate Cache (1 = Redirect stores to Cache, 0 = To RAM)
#define SR_Swc      (1 << 17) // SwC: Swap Caches
#define SR_PZ       (1 << 18) // PZ:  Pound Zero (Cache parity bits)
#define SR_CM       (1 << 19) // CM:  Cache Miss status
#define SR_PE       (1 << 20) // PE:  Parity Error
#define SR_TS       (1 << 21) // TS:  TLB Shutdown
#define SR_BEV      (1 << 22) // BEV: Boot Exception Vectors (0 = 0x80000080, 1 = 0xBFC00180)

// 4. Coprocessor Usability (Bits 28-31)
#define SR_CU0      (1 << 28) // Coprocessor 0 Enable (Always usable in Kernel mode)
#define SR_CU1      (1 << 29) // Coprocessor 1 Enable (FPU - Unused on PSX)
#define SR_CU2      (1 << 30) // Coprocessor 2 Enable (GTE - PSX Geometry Transformation Engine)
#define SR_CU3      (1 << 31) // Coprocessor 3 Enable (Unused)

#define cp0_epc cp0_regs[CP0_REG_EPC]

// Standard MIPS-I Coprocessor 0 Hardware Register Indices
enum {
    CP0_REG_INDEX    = 0,   // TLB Index
    CP0_REG_RANDOM   = 1,   // TLB Random
    CP0_REG_ENTRYLO  = 2,   // TLB EntryLo
    CP0_REG_CONTEXT  = 4,   // Context
    CP0_REG_BADVADDR = 8,   // Bad Virtual Address
    CP0_REG_PRID     = 15,  // Processor ID

    // --- The 3 Essential Core Registers for your validation suite & PSX ---
    CP0_REG_SR       = 12,  // Status Register (replaces custom sub-variables)
    CP0_REG_CAUSE    = 13,  // Exception Cause Register
    CP0_REG_EPC      = 14,  // Exception Program Counter

    CP0_MAX_REGS     = 32   // MIPS defines exactly 32 structural registers
};

typedef struct {
    uint32_t regs[32];

    // Multiply / divide result registers.
    uint32_t hi;
    uint32_t lo;

    // Program counter model. PC is the instruction about to run; next_pc is the
    // instruction after it. A taken branch overwrites next_pc, which leaves the
    // instruction already sitting at PC to run as the branch delay slot. This
    // two-register form handles delay slots without a separate state machine.
    uint32_t PC;
    uint32_t next_pc;
    uint32_t current_pc; // PC of the instruction currently executing (for EPC)

    // Branch tracking. branch_taken is set when the current instruction takes a
    // branch; in_delay_slot records that the previous instruction did, so the
    // current one occupies a delay slot (needed for exception reporting).
    uint8_t  branch_taken;
    uint8_t  in_delay_slot;

    // Load delay variables
    uint32_t load_reg;   // The register slated to be updated (0 if none)
    uint32_t load_val;   // The value that will be written

    // PSX Interrupt Controller Registers (Physical addresses 0x1F801070 and 0x1F801074)
    uint32_t i_stat; // Interrupt Status (which hardware lines are actively pulling)
    uint32_t i_mask; // Interrupt Mask (which lines the CPU allows to pass through)

    // Coprocessor 0 (CP0) System Registers Array
    // Though there are 32 possible register indices, we only care about a few:
    // Index 12: SR (Status Register)
    // Index 13: CAUSE (Exception Cause Register)
    // Index 14: EPC (Exception Program Counter)
    uint32_t cp0_regs[32];

    // Set when the CPU executes BREAK or SYSCALL. The run loops stop stepping
    // once this is set. Used by the headless test harness to know a program
    // has signalled completion.
    uint8_t  halted;
} R3051_CPU;

extern R3051_CPU cpu;

void cpu_reset(uint32_t start_pc);
void step_cpu(void);
void check_hardware_interrupts(void);

#endif
