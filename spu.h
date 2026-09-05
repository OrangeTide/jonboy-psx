#ifndef SPU_H_
#define SPU_H_
#include <stdint.h>

// The SPU register block (0x1F801C00 - 0x1F801FFF). This is only a register
// stub: it stores writes and reads them back, which is enough for the BIOS SPU
// init handshake. No audio is generated.
#define SPU_BASE 0x1F801C00
#define SPU_SIZE 0x400

uint32_t psx_spu_read(uint32_t phys_addr, int size);
void psx_spu_write(uint32_t phys_addr, uint32_t value, int size);
void psx_spu_reset(void);

#endif
