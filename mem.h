#ifndef MEM_H_
#define MEM_H_

#include "rom.h"
#include <stdint.h>

#define MEM_MAX_SIZE (4096 * 1024)
extern uint8_t system_ram[MEM_MAX_SIZE];

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
// physical and hardware registers are dispatched.
uint32_t memory_read_word(uint32_t v_addr);
void memory_write_word(uint32_t v_addr, uint32_t value);

// Load a flat binary file straight into system RAM at a physical address.
// Returns 0 on success, negative on error.
int mem_load_file(const char *filename, uint32_t phys_addr);
#endif
