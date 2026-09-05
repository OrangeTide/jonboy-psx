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
#endif
