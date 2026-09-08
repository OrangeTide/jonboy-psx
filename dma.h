#ifndef DMA_H_
#define DMA_H_
#include <stdint.h>

typedef struct {
    uint32_t madr; // Base Memory Address
    uint32_t bcr;  // Block Count and Block Size Configuration
    uint32_t chcr; // Channel Control (Triggers and directs the transfer)
} DMA_Channel;

extern DMA_Channel dma_channels[7];
extern uint32_t dma_control;   // Global DMA Control Register (0x1F8010F0)
extern uint32_t dma_interrupt; // Global DMA Interrupt Register (0x1F8010F4)

void psx_dma_tick(uint32_t cycles);
void run_dma_transfer(int channel);

// Cycles until a deferred DMA completion interrupt is due, or UINT32_MAX when
// none is pending.
uint32_t dma_next_event(void);
#endif
