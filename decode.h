// Extract fields from a fetched 32-bit instruction
#define GET_OPCODE(inst) ((inst) >> 26)          // Top 6 bits
#define GET_RS(inst)     (((inst) >> 21) & 0x1F) // 5 bits (Source Reg 1)
#define GET_RT(inst)     (((inst) >> 16) & 0x1F) // 5 bits (Source Reg 2 / Target)
#define GET_RD(inst)     (((inst) >> 11) & 0x1F) // 5 bits (Destination Reg)
#define GET_SHAMT(inst)  (((inst) >> 6) & 0x1F)  // 5 bits (Shift Amount)
#define GET_FUNCT(inst)  ((inst) & 0x3F)         // Bottom 6 bits (ALU function)

// Immediate Fields
#define GET_IMM16(inst)  ((inst) & 0xFFFF)       // 16-bit Immediate value
#define GET_TARGET(inst) ((inst) & 0x3FFFFFF)    // 26-bit Jump Target

// Sign Extension Helper for Immediates
// If bit 15 is 1, fills top 16 bits with 1s. Otherwise, fills with 0s.
#define SIGN_EXTEND_16(imm) ((int32_t)(int16_t)(imm))

