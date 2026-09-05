#ifndef MMIO_H_
#define MMIO_H_
// Strips virtual segment prefixes (0x8xxxxxxx / 0xAxxxxxxx) to 32-bit physical space
#define TO_PHYSICAL(addr) ((addr) & 0x1FFFFFFF)

// PSX MMIO Physical Boundary Constants
#define MMIO_BASE   0x1F800000
#define MMIO_LENGTH 0x00003000 // Covers up to 0x1F802FFF
#endif
