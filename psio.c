#include "psio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PsioDevice psio;

bool
load_psio_firmware(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");

    if (!f) {
        perror(filepath);
        return false;
    }

    if (!psio.firmware_rom) {
        psio.firmware_rom = malloc(PSIO_EXP1_SIZE);

        if (!psio.firmware_rom) {
            fprintf(stderr, "psio firmware: out of memory\n");
            fclose(f);
            return false;
        }
    }

    // Pad the whole window with zero so bytes past the image read as 0.
    memset(psio.firmware_rom, 0, PSIO_EXP1_SIZE);

    size_t got = fread(psio.firmware_rom, 1, PSIO_EXP1_SIZE, f);
    fclose(f);

    if (got == 0) {
        fprintf(stderr, "%s: empty or unreadable firmware\n", filepath);
        return false;
    }

    return true;
}

bool
load_virtual_sd_card(const char *filepath)
{
    FILE *f = fopen(filepath, "rb");

    if (!f) {
        perror(filepath);
        return false;
    }

    // Keep the handle open: DMA and FIFO reads stream from it later. Close any
    // image that was already mounted first.
    if (psio.sd_card_file) {
        fclose(psio.sd_card_file);
    }

    psio.sd_card_file = f;

    return true;
}

void
process_psio_command(void)
{
    // TODO: decode psio.command_reg (mount, seek, read sector, report status)
    // and drive the FIFO / DMA accordingly. For now just acknowledge the
    // command by clearing BUSY so the guest does not stall.
    psio.status_reg &= ~0x01u;
}

// Little-endian load of a 1, 2, or 4 byte value from a byte buffer.
static uint32_t
le_load(const uint8_t *b, uint32_t off, int size)
{
    uint32_t v = 0;

    for (int i = 0; i < size; i++) {
        v |= (uint32_t)b[off + i] << (i * 8);
    }

    return v;
}

uint32_t
psio_read(uint32_t phys_addr, int size)
{
    // Firmware ROM window (Expansion Region 1).
    if (phys_addr >= PSIO_EXP1_BASE && phys_addr < PSIO_EXP1_BASE + PSIO_EXP1_SIZE) {
        if (!psio.firmware_rom) {
            return 0xFFFFFFFF; // unmapped ROM reads as all-ones
        }

        return le_load(psio.firmware_rom, phys_addr - PSIO_EXP1_BASE, size);
    }

    // Device registers (Expansion Region 2).
    switch (phys_addr) {
    case PSIO_REG_STATUS:
        return psio.status_reg;

    case PSIO_REG_FIFO: // pop from the SD data FIFO
        if (psio.fifo_count >= (uint32_t)size
                && psio.fifo_ptr + (uint32_t)size <= sizeof(psio.fifo_buffer)) {
            uint32_t data = le_load(psio.fifo_buffer, psio.fifo_ptr, size);
            psio.fifo_ptr += (uint32_t)size;
            psio.fifo_count -= (uint32_t)size;
            return data;
        }

        return 0; // FIFO empty

    default:
        return 0;
    }
}

void
psio_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)size; // device registers are word-oriented for now

    switch (phys_addr) {
    case PSIO_REG_STATUS: // command register on the write side
        psio.command_reg = value;
        process_psio_command();
        break;

    case PSIO_REG_LBA: // sector address
        // TODO: map the LBA to a byte offset and fseek psio.sd_card_file.
        break;

    default:
        // Firmware ROM window and unknown registers ignore writes.
        break;
    }
}
