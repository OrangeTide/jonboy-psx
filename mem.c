#include "mem.h"
#include "mmio.h"
#include "dma.h"
#include "cpu.h"
#include "pad.h"
#include "psio.h"
#include "spu.h"
#include "cdrom.h"
#include "timer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t system_ram[MEM_MAX_SIZE];

uint32_t read_hardware_word(uint32_t physical_addr);
void write_hardware_word(uint32_t physical_addr, uint32_t value);

uint32_t memory_read_word(uint32_t v_addr) {
    uint32_t p_addr = TO_PHYSICAL(v_addr);

    // Check if the address hits the PlayStation MMIO block
    if (p_addr >= MMIO_BASE && p_addr < (MMIO_BASE + MMIO_LENGTH)) {
        return read_hardware_word(p_addr);
    }

    // Fallback to standard RAM fetch macro
    return MEM_READ_32(p_addr);
}

void memory_write_word(uint32_t v_addr, uint32_t value) {
    uint32_t p_addr = TO_PHYSICAL(v_addr);

    if (p_addr >= MMIO_BASE && p_addr < (MMIO_BASE + MMIO_LENGTH)) {
        write_hardware_word(p_addr, value);
        return;
    }

    MEM_WRITE_32(p_addr, value);
}

uint32_t
read_hardware_word(uint32_t physical_addr)
{
    uint32_t offset = physical_addr - MMIO_BASE;

    // 1. Core Interconnect & Memory Control Registers (0x1F801000 - 0x1F801020)
    if (offset >= 0x1000 && offset <= 0x1020) {
        // Return memory timings / expansion parameters
        return 0x00000000;
    }

    // used inside MMIO dispatcher handlers:
    if (offset >= 0x1080 && offset <= 0x10E8) {
        uint32_t channel = (offset - 0x1080) / 0x10;
        uint32_t reg     = (offset - 0x1080) & 0x0F;
        if (reg == 0x0) return dma_channels[channel].madr;
        if (reg == 0x4) return dma_channels[channel].bcr;
        if (reg == 0x8) return dma_channels[channel].chcr;
    }


    switch (offset) {
        // 2. CONTROLLERS & MEMORY CARDS (0x1F801040)
        case 0x1040:
            return psx_pad_read_data();
        case 0x1044:
            return psx_pad_read_stat();

        // 3. INTERRUPT CONTROLLER (0x1F801070 - 0x1F801074)
        case 0x1070: return cpu.i_stat;
        case 0x1074: return cpu.i_mask;

        // 3. DMA CONTROLLER (0x1F8010F0 - 0x1F8010F4)
        case 0x10F0: return cpu.dma_control;
        case 0x10F4: return cpu.dma_interrupt;

        // 4. TIMERS (0x1F801100 - 0x1F801128)
        case 0x1100: return psx_timer_get_value(0);
        case 0x1104: return psx_timer_get_mode(0);

        // 5. GRAPHICS PROCESSING UNIT (GPU) (0x1F801810 - 0x1F801814)
        case 0x1810:
            return psx_gpu_read_vram_or_gp0();
        case 0x1814:
            return psx_gpu_get_status(); // Vital for frame synchronization games

        default:
            // Fail silently or log unhandled hardware registers
            // printf("Unhandled MMIO Word Read: 0x%08X\n", physical_addr);
            return 0;
    }
}

void write_hardware_word(uint32_t physical_addr, uint32_t value) {
    uint32_t offset = physical_addr - MMIO_BASE;

    if (offset >= 0x1080 && offset <= 0x10E8) {
        uint32_t channel = (offset - 0x1080) / 0x10;
        uint32_t reg     = (offset - 0x1080) & 0x0F;

        if (reg == 0x0) dma_channels[channel].madr = value;
        if (reg == 0x4) dma_channels[channel].bcr  = value;
        if (reg == 0x8) {
            dma_channels[channel].chcr = value;
            // If bit 24 is set, a DMA transfer has been triggered!
            if ((value & 0x01000000) != 0) {
                run_dma_transfer(channel);
            }
        }
    }

    switch (offset) {
        // Interrupt Control Register (0x1F801070 / 0x1F801074)
        case 0x1070:
            // psx_acknowledge_interrupts(value);
            cpu.i_stat &= ~value;
            break;
        case 0x1074:
            // psx_set_interrupt_mask(value);
            cpu.i_mask = value;
            break;

        // GPU Commands (GP0 / GP1)
        case 0x1810:
            psx_gpu_send_command_gp0(value);
            break;
        case 0x1814:
            psx_gpu_send_command_gp1(value);
            break;

        // DMA Channels (Example: GPU DMA, CDROM DMA)
        case 0x10A8: psx_dma_set_channel_base(0, value); break;
        case 0x10AC: psx_dma_set_channel_block(0, value); break;
        case 0x10B0: psx_dma_trigger_channel(0, value); break;

        default:
            // printf("Unhandled MMIO Word Write: 0x%08X = 0x%08X\n", physical_addr, value);
            break;
    }
}
