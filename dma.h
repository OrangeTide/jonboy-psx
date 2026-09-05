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

void run_dma_transfer(int channel);
#endif
