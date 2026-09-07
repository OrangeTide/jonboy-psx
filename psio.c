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
    psio.status_reg = PSIO_STATUS_READY; // a freshly mounted card is idle

    return true;
}

// Position the mounted image at the current LBA. Both the FIFO read command and
// a DMA channel-3 transfer stream from wherever the file pointer sits, so the
// LBA write and any read must agree on this seek. Returns 0 on success.
static int
psio_seek_to_lba(void)
{
    if (!psio.sd_card_file) {
        return -1;
    }

    long off = (long)((uint64_t)psio.lba * PSIO_SECTOR_SIZE);

    return fseek(psio.sd_card_file, off, SEEK_SET);
}

// Read one sector at the current LBA into the FIFO, then advance the LBA so
// sequential reads walk the image. Sets DATA on success or ERROR on failure.
static void
psio_read_sector_to_fifo(void)
{
    psio.fifo_ptr = 0;
    psio.fifo_count = 0;
    psio.status_reg &= ~(PSIO_STATUS_DATA | PSIO_STATUS_ERROR);

    if (psio_seek_to_lba() != 0) {
        psio.status_reg |= PSIO_STATUS_ERROR;
        return;
    }

    size_t got = fread(psio.fifo_buffer, 1, PSIO_SECTOR_SIZE, psio.sd_card_file);

    if (got == 0) {
        psio.status_reg |= PSIO_STATUS_ERROR; // read past the end of the image
        return;
    }

    // Zero-fill a short trailing sector so reads past EOF stay defined.
    if (got < PSIO_SECTOR_SIZE) {
        memset(psio.fifo_buffer + got, 0, PSIO_SECTOR_SIZE - got);
    }

    psio.fifo_count = PSIO_SECTOR_SIZE;
    psio.lba++;
    psio.status_reg |= PSIO_STATUS_DATA;
}

void
process_psio_command(void)
{
    switch (psio.command_reg) {
    case PSIO_CMD_READ_SECTOR:
        psio_read_sector_to_fifo();
        break;

    case PSIO_CMD_NOP:
    default:
        // Unknown or probe commands complete with no side effects.
        break;
    }

    psio.status_reg |= PSIO_STATUS_READY; // the command has been handled
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

            if (psio.fifo_count == 0) {
                psio.status_reg &= ~PSIO_STATUS_DATA; // FIFO drained
            }

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
        psio.lba = value;
        // Seek now so a DMA channel-3 read (which streams from the current file
        // position) lands on this sector without a separate read command.
        psio_seek_to_lba();
        break;

    default:
        // Firmware ROM window and unknown registers ignore writes.
        break;
    }
}
