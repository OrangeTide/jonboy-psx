#include "cpu.h"
#include "dma.h"
#include "gpu.h"
#include "mem.h"
#include "psio.h"
#include "cdrom.h"
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
}

// SPU DMA (channel 4): the SPU sound RAM is not modeled yet, so a write to the
// SPU just consumes the words and a read back fills RAM with zeros. Completing
// the transfer and raising the SPU interrupt is what unblocks a game that waits
// on the SPU event (HwSPU) after kicking a sound upload.
static void
dma_spu(int channel)
{
    uint32_t chcr = dma_channels[channel].chcr;
    uint32_t bcr  = dma_channels[channel].bcr;
    uint32_t madr = dma_channels[channel].madr & RAM_MASK;

    uint32_t block_size  = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    uint32_t words = block_size * block_count;

    if (words == 0) {
        words = block_size;
    }

    // Direction is CHCR bit 0: 1 = RAM to device (a sound upload). Only a read
    // back needs to touch RAM, and it has nothing real to return yet.
    if ((chcr & 1) == 0) {
        for (uint32_t i = 0; i < words; i++) {
            MEM_WRITE_32((madr + i * 4) & RAM_MASK, 0);
        }
    }
}

// CD-ROM DMA (channel 3): copy the current sector's data words into RAM. The
// word count comes from BCR the same way as the card transfer above.
static void
dma_cdrom_read(int channel)
{
    uint32_t chcr = dma_channels[channel].chcr;
    uint32_t bcr  = dma_channels[channel].bcr;
    uint32_t madr = dma_channels[channel].madr & RAM_MASK;

    if ((chcr & 1) != 0) {
        return; // only device to RAM makes sense
    }

    uint32_t block_size  = bcr & 0xFFFF;
    uint32_t block_count = (bcr >> 16) & 0xFFFF;
    uint32_t words = block_size * block_count;

    if (words == 0) {
        words = block_size;
    }

    cdrom_dma_read(madr, words);
}

// Walk a GPU command linked list (SyncMode 2). Each node is a header word whose
// top 8 bits give the word count and whose low 24 bits point at the next node;
// the words after the header are GP0 commands. The list ends when the header's
// bit 23 is set (0xFFFFFF is the usual terminator).
static void
dma_gpu_linked_list(uint32_t addr)
{
    // Bound the walk so a malformed list cannot loop forever.
    for (int guard = 0; guard < 0x100000; guard++) {
        uint32_t header = MEM_READ_32(addr & RAM_MASK);
        uint32_t count = header >> 24;

        for (uint32_t i = 0; i < count; i++) {
            addr = (addr + 4) & RAM_MASK;
            psx_gpu_send_command_gp0(MEM_READ_32(addr));
        }

        if (header & 0x800000) {
            break; // end-of-list marker
        }

        addr = header & 0x1FFFFC; // next node, word-aligned and masked to RAM
    }
}

// Transfer a fixed number of words between RAM and the GPU (SyncMode 0/1).
// from_ram sends GP0 commands; otherwise VRAM data is read back into RAM.
static void
dma_gpu_block(uint32_t addr, uint32_t words, int from_ram, int decrement)
{
    int step = decrement ? -4 : 4;

    for (uint32_t i = 0; i < words; i++) {
        addr &= RAM_MASK;

        if (from_ram) {
            psx_gpu_send_command_gp0(MEM_READ_32(addr));
        } else {
            MEM_WRITE_32(addr, psx_gpu_read_vram_or_gp0());
        }

        addr += (uint32_t)step;
    }
}

// GPU DMA (channel 2): linked-list command lists or block transfers.
static void
dma_gpu(int channel)
{
    uint32_t chcr = dma_channels[channel].chcr;
    uint32_t bcr  = dma_channels[channel].bcr;
    uint32_t madr = dma_channels[channel].madr;
    uint32_t sync = (chcr >> 9) & 3;
    int from_ram  = chcr & 1;
    int decrement = (chcr >> 1) & 1;

    if (sync == 2) {
        dma_gpu_linked_list(madr);
    } else if (sync == 1) {
        // Block mode: BCR is block size (low 16) times block count (high 16).
        uint32_t words = (bcr & 0xFFFF) * ((bcr >> 16) & 0xFFFF);
        dma_gpu_block(madr, words, from_ram, decrement);
    } else {
        // Immediate mode: BCR low 16 bits is the word count.
        dma_gpu_block(madr, bcr & 0xFFFF, from_ram, decrement);
    }
}

// Cycles to wait before a completed DMA pulses the CPU interrupt line.
#define DMA_IRQ_DELAY 700

static int dma_irq_delay;

// Advance the deferred DMA completion interrupt and pull the CPU line when it
// expires. Called from the hardware tick.
void
psx_dma_tick(uint32_t cycles)
{
    if (dma_irq_delay > 0) {
        dma_irq_delay -= (int)cycles;
        if (dma_irq_delay <= 0) {
            dma_irq_delay = 0;
            if (dma_interrupt & 0x80000000u) {
                cpu.i_stat |= (1u << IRQ_DMA);
            }
        }
    }
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
        // Defer signaling the CPU. The transfer runs synchronously inside the
        // triggering register write, but on real hardware the completion
        // interrupt arrives a little later, after the code that kicked the DMA
        // has returned. Firing it immediately would re-enter the game mid-setup.
        dma_irq_delay = DMA_IRQ_DELAY;
    }
}

void
run_dma_transfer(int channel)
{
    switch (channel) {
    case 2: // GPU command lists / VRAM transfers
        dma_gpu(channel);
        break;

    case 3: // CD-ROM slot: a mounted disc serves sectors, else the PSIO card
        if (cdrom_has_disc()) {
            dma_cdrom_read(channel);
        } else {
            dma_psio_read(channel);
        }
        break;

    case 4: // SPU sound upload / download
        dma_spu(channel);
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
