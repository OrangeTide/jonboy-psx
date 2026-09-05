#include "cpu.h"
#include "dma.h"
#include "mem.h"
#include "psio.h"
#include <string.h>

DMA_Channel dma_channels[7];
uint32_t dma_control;   // Global DMA Control Register (0x1F8010F0)
uint32_t dma_interrupt; // Global DMA Interrupt Register (0x1F8010F4)

// Ordering-Table Clear (channel 6). Writes a backward-linked list of pointers
// into RAM, terminated by the end marker, which the GPU DMA later walks.
static void
dma_otc_clear(int channel)
{
    uint32_t chcr = dma_channels[channel].chcr;
    uint32_t mode = (chcr >> 9) & 3;

    if (mode != 0) {
        return; // only the linked-list clear mode is handled
    }

    uint32_t addr  = dma_channels[channel].madr & RAM_MASK;
    uint32_t words = dma_channels[channel].bcr & 0xFFFF;

    for (uint32_t i = 0; i < words; i++) {
        uint32_t next_pointer = (i == words - 1) ? 0x00FFFFFF : (addr - 4) & 0xFFFFFF;
        MEM_WRITE_32(addr & RAM_MASK, next_pointer);
        addr -= 4; // OTC builds the list backward through memory
    }
}

// PSIO SD-card read (channel 3, the CD-ROM slot the PSIO cartridge replaces).
// Streams words from the mounted image into main RAM.
static void
dma_psio_read(int channel)
{
    uint32_t chcr = dma_channels[channel].chcr;
    uint32_t bcr  = dma_channels[channel].bcr;
    uint32_t madr = dma_channels[channel].madr & RAM_MASK;

    // Direction is CHCR bit 0: 0 = device to RAM (a read). Only that direction
    // makes sense for loading from the card, so ignore the write direction.
    if ((chcr & 1) != 0) {
        return;
    }

    if (!psio.sd_card_file) {
        return; // no SD image mounted
    }

    // Block mode: low 16 bits are words per block, high 16 bits the block
    // count. A count of zero means a single block set only the low half.
    uint32_t block_size  = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    uint32_t words = block_size * block_count;

    if (words == 0) {
        words = block_size;
    }

    size_t bytes = (size_t)words * 4;

    // Clamp so a large transfer cannot run past the end of RAM.
    if ((uint64_t)madr + bytes > RAM_SIZE) {
        bytes = RAM_SIZE - madr;
    }

    size_t got = fread(&system_ram[madr], 1, bytes, psio.sd_card_file);

    if (got < bytes) {
        // Short read (EOF or error): zero the remainder so RAM stays defined.
        memset(&system_ram[madr + got], 0, bytes - got);
    }

    psio.status_reg &= ~0x01u; // clear BUSY
}

// Shared completion path: clear the channel's start bit and, if the channel's
// DMA interrupt is enabled, raise its flag and signal the CPU.
static void
dma_finish(int channel)
{
    dma_channels[channel].chcr &= ~0x01000000u;

    // DICR layout: bit 23 master enable, bits 16-22 per-channel IRQ enable,
    // bits 24-30 per-channel IRQ flags, bit 31 master flag.
    uint32_t master_enable  = (dma_interrupt >> 23) & 1;
    uint32_t channel_enable = (dma_interrupt >> (16 + channel)) & 1;

    if (master_enable && channel_enable) {
        dma_interrupt |= (1u << (24 + channel)); // raise this channel's IRQ flag
        dma_interrupt |= (1u << 31);             // raise the master IRQ flag
        cpu.i_stat |= (1u << 3);                 // signal the DMA line to the CPU
    }
}

void
run_dma_transfer(int channel)
{
    switch (channel) {
    case 3: // CD-ROM slot, driven by the PSIO cartridge
        dma_psio_read(channel);
        break;

    case 6: // Ordering-table clear
        dma_otc_clear(channel);
        break;

    default:
        // Other channels (GPU, SPU, ...) are not implemented yet. Fall through
        // to the completion hooks so the guest is not left waiting on the start
        // bit forever.
        break;
    }

    dma_finish(channel);
}

void
psx_dma_set_channel_base(int channel, uint32_t value)
{
    if (channel >= 0 && channel < 7) {
        dma_channels[channel].madr = value;
    }
}

void
psx_dma_set_channel_block(int channel, uint32_t value)
{
    if (channel >= 0 && channel < 7) {
        dma_channels[channel].bcr = value;
    }
}

void
psx_dma_trigger_channel(int channel, uint32_t value)
{
    if (channel >= 0 && channel < 7) {
        dma_channels[channel].chcr = value;

        // Bit 24 represents the instantaneous hardware trigger request flag
        if ((value & 0x01000000) != 0) {
            run_dma_transfer(channel);
        }
    }
}
