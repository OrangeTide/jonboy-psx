#ifndef MDEC_H_
#define MDEC_H_
#include <stdint.h>

// Macroblock Decoder (MDEC). Two 32-bit registers live in the general MMIO
// block: 0x1F801820 is the command/data port (write a command word, then its
// parameter words; read pops decoded output), and 0x1F801824 is the
// control/status port (write resets and enables the DMA request lines, read
// returns the status word). Bulk data moves through DMA channel 0 (RAM into
// the decoder) and channel 1 (decoded pixels back to RAM).
#define MDEC_BASE 0x1F801820
#define MDEC_SIZE 0x8

void mdec_reset(void);

// MMIO register access. phys_addr selects the port by bit 2: 0x1820 is the
// data/command port, 0x1824 is the control/status port.
uint32_t mdec_read(uint32_t phys_addr);
void mdec_write(uint32_t phys_addr, uint32_t value);

// DMA seam. Channel 0 pushes one input word per call; channel 1 pops one
// output word per call. These are the data port under its DMA-side names.
void mdec_dma_in(uint32_t word);
uint32_t mdec_dma_out(void);

// True while decoded output words remain for DMA channel 1 to drain.
int mdec_dma_out_ready(void);
#endif
