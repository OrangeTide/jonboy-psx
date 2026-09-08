#ifndef MEM_H_
#define MEM_H_

#include "rom.h"
#include <stdint.h>

// PlayStation memory regions (physical addresses after segment stripping).
#define RAM_SIZE     (2 * 1024 * 1024) // 2 MB main RAM, mirrored across 0x00000000-0x007FFFFF
#define RAM_MASK     (RAM_SIZE - 1)
#define RAM_END      0x00800000        // end of the mirrored RAM window
#define SCRATCH_BASE 0x1F800000        // 1 KB scratchpad (D-cache used as fast RAM)
#define SCRATCH_SIZE 0x00000400
#define BIOS_BASE    0x1FC00000        // 512 KB BIOS ROM
#define BIOS_SIZE    0x00080000

// Backwards-compatible alias: the flat-load harness uses this as the RAM bound.
#define MEM_MAX_SIZE RAM_SIZE
extern uint8_t system_ram[MEM_MAX_SIZE];
extern uint8_t bios_rom[BIOS_SIZE];

// 1. Fetch a 32-bit Word (Little-Endian)
// Combines 4 bytes: address A is the lowest byte, A+3 is the highest byte.
#define MEM_READ_32(addr) ( \
    ((uint32_t)(system_ram[(addr) + 0]))       | \
    ((uint32_t)(system_ram[(addr) + 1] << 8))  | \
    ((uint32_t)(system_ram[(addr) + 2] << 16)) | \
    ((uint32_t)(system_ram[(addr) + 3] << 24))   \
)

// 2. Fetch a 16-bit Halfword (Little-Endian)
#define MEM_READ_16(addr) ( \
    ((uint16_t)(system_ram[(addr) + 0]))       | \
    ((uint16_t)(system_ram[(addr) + 1] << 8))    \
)

// 3. Write a 32-bit Word (Little-Endian)
// Splits a 32-bit value into 4 individual bytes across sequential memory addresses.
#define MEM_WRITE_32(addr, val) do { \
    system_ram[(addr) + 0] = (uint8_t)((val) & 0xFF);         \
    system_ram[(addr) + 1] = (uint8_t)(((val) >> 8) & 0xFF);  \
    system_ram[(addr) + 2] = (uint8_t)(((val) >> 16) & 0xFF); \
    system_ram[(addr) + 3] = (uint8_t)(((val) >> 24) & 0xFF); \
} while(0)

// 4. Write a 16-bit Halfword (Little-Endian)
#define MEM_WRITE_16(addr, val) do { \
    system_ram[(addr) + 0] = (uint8_t)((val) & 0xFF);         \
    system_ram[(addr) + 1] = (uint8_t)(((val) >> 8) & 0xFF);  \
} while(0)

// Address-translating, MMIO-aware accessors. The CPU must go through these,
// never the raw macros above, so that virtual addresses are converted to
// physical, the region is decoded, and hardware registers are dispatched.
uint32_t memory_read_word(uint32_t v_addr);
uint32_t memory_read_halfword(uint32_t v_addr);
uint32_t memory_read_byte(uint32_t v_addr);
void memory_write_word(uint32_t v_addr, uint32_t value);
void memory_write_halfword(uint32_t v_addr, uint32_t value);
void memory_write_byte(uint32_t v_addr, uint32_t value);

// Instruction fetch: reads a word like memory_read_word but never invokes the
// watchpoint hook, so a data watchpoint does not trip on the fetch of the code
// it is watching. The CPU fetch path uses this.
uint32_t mem_fetch_word(uint32_t v_addr);

// Optional debugger watchpoint hook. When non-NULL, the CPU data accessors above
// call it on each load/store with the address, size in bytes, and whether it is
// a write, so the gdb server can detect a watched access. NULL (the default) adds
// only a single predictable branch per access, and instruction fetch skips it.
extern void (*mem_watch_hook)(uint32_t addr, int size, int is_write);

// Load a flat binary file straight into system RAM at a physical address.
// Returns 0 on success, negative on error.
int mem_load_file(const char *filename, uint32_t phys_addr);

// Load a BIOS image into the mapped BIOS region. Returns 0 on success.
int mem_load_bios(const char *filename);

// Load a PS-EXE: parse the header, copy the image into RAM, and return the
// entry PC, initial GP, and initial SP. Returns 0 on success.
int mem_load_exe(const char *filename, uint32_t *out_pc, uint32_t *out_gp, uint32_t *out_sp);
#endif
