#include "cpu.h"
#include "dma.h"
#include "mem.h"

DMA_Channel dma_channels[7];
uint32_t dma_control;   // Global DMA Control Register (0x1F8010F0)
uint32_t dma_interrupt; // Global DMA Interrupt Register (0x1F8010F4)

void
run_dma_transfer(int channel)
{
    uint32_t addr  = dma_channels[channel].madr & 0x1FFFFF; // Mask to RAM bounds
    uint32_t chcr  = dma_channels[channel].chcr;

    // Direction: 0 = To RAM from Peripheral, 1 = From RAM to Peripheral
    uint32_t direction = chcr & 1;

    if (channel == 6) { // OTC Channel (Used heavily to clear drawing tables)
        uint32_t mode = (chcr >> 9) & 3;

        if (mode == 0) { // Clear/Initialize Ordering Table
            uint32_t words = dma_channels[channel].bcr & 0xFFFF;

            // Loop through memory instantly and write hardware links
            for (uint32_t i = 0; i < words; i++) {
                uint32_t next_pointer = (i == words - 1) ? 0x00FFFFFF : (addr - 4) & 0xFFFFFF;
                MEM_WRITE_32(addr, next_pointer);
                addr -= 4; // OTC works backwards through memory addresses
            }
        }
    }

    // ---- TRANSFER COMPLETE HOOKS ----
    // 1. Clear the active trigger bit in the channel control register
    dma_channels[channel].chcr &= ~0x01000000;

    // 2. Set the completion flag inside the Global DMA Interrupt register
    cpu.dma_interrupt |= (1 << (16 + channel));

    // 3. Trigger the Master Hardware Interrupt if the channel's DMA interrupt mask is active
    uint32_t master_dma_enable = (cpu.dma_interrupt >> 23) & 1;
    uint32_t channel_mask      = (cpu.dma_interrupt >> (16 + channel)) & 1;

    if (master_dma_enable && channel_mask) {
        cpu.i_stat |= (1 << 3); // Bit 3 of I_STAT is explicitly reserved for DMA completion
    }
}
