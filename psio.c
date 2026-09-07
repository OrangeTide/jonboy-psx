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
    // A freshly mounted card is idle and ready to take a command.
    psio.status_reg = PSIO_STAT_TX_FREE | PSIO_STAT_SD_INSERTED;
    psio.tx_count = 0;
    psio.fifo_ptr = psio.fifo_count = psio.stream_remaining = 0;

    return true;
}

// Stage the next chunk of the active read into the FIFO buffer, refilling only
// when it has been drained. Clears RX_READY once the whole read is delivered.
static void
psio_refill_staging(void)
{
    if (psio.fifo_count > 0) {
        return; // still bytes to hand out
    }

    if (psio.stream_remaining == 0 || !psio.sd_card_file) {
        psio.status_reg &= ~PSIO_STAT_RX_READY;
        return;
    }

    uint32_t chunk = psio.stream_remaining;

    if (chunk > sizeof(psio.fifo_buffer)) {
        chunk = sizeof(psio.fifo_buffer);
    }

    size_t got = fread(psio.fifo_buffer, 1, chunk, psio.sd_card_file);

    // Zero-fill a short read (EOF) so the transfer size is still honored.
    if (got < chunk) {
        memset(psio.fifo_buffer + got, 0, chunk - got);
    }

    psio.fifo_ptr = 0;
    psio.fifo_count = chunk;
    psio.stream_remaining -= chunk;
    psio.status_reg |= PSIO_STAT_RX_READY;
}

// Run the command packet assembled in tx_words. The header holds the id, flags,
// and block count; word 1 is the LBA; word 2 is an XOR checksum of the first
// two. A bad checksum or unknown id is ignored so a stray strobe cannot wedge
// the device.
void
process_psio_command(void)
{
    uint32_t header = psio.tx_words[0];
    uint32_t lba    = psio.tx_words[1];
    uint32_t check  = psio.tx_words[2];

    psio.tx_count = 0;
    psio.status_reg |= PSIO_STAT_TX_FREE;

    if (check != (header ^ lba)) {
        return; // malformed packet, checksum mismatch
    }

    uint32_t cmd    = header & 0xFF;
    uint32_t flags  = (header >> 8) & 0xFF;
    uint32_t blocks = (header >> 16) & 0xFFFF;

    if (blocks == 0) {
        blocks = 1;
    }

    if (cmd != PSIO_CMD_READ_SECTOR || !psio.sd_card_file) {
        return;
    }

    // Seek the image to the requested sector. A polling read stages data into
    // the FIFO; a DMA read leaves the file positioned for channel 3 to stream.
    if (fseek(psio.sd_card_file, (long)((uint64_t)lba * PSIO_SECTOR_SIZE),
              SEEK_SET) != 0) {
        return;
    }

    psio.fifo_ptr = psio.fifo_count = 0;

    if (flags & PSIO_FLAG_DMA) {
        psio.stream_remaining = 0; // DMA channel 3 reads straight from the file
        return;
    }

    psio.stream_remaining = blocks * PSIO_SECTOR_SIZE;
    psio_refill_staging();
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
    // The device registers sit at the base of the window and shadow the first
    // bytes of the ROM, so they are decoded before the ROM fallback below.
    switch (phys_addr) {
    case PSIO_REG_DATA: { // pop the next word/bytes of streamed data
        psio_refill_staging();

        if (psio.fifo_count >= (uint32_t)size
                && psio.fifo_ptr + (uint32_t)size <= sizeof(psio.fifo_buffer)) {
            uint32_t data = le_load(psio.fifo_buffer, psio.fifo_ptr, size);
            psio.fifo_ptr += (uint32_t)size;
            psio.fifo_count -= (uint32_t)size;

            if (psio.fifo_count == 0 && psio.stream_remaining == 0) {
                psio.status_reg &= ~PSIO_STAT_RX_READY; // whole read delivered
            }

            return data;
        }

        return 0; // no data staged
    }

    case PSIO_REG_STATUS:
        return psio.status_reg;

    case PSIO_REG_IRQ:
        return psio.irq_reg;
    }

    // Firmware ROM window (the rest of Expansion Region 1).
    if (phys_addr >= PSIO_EXP1_BASE && phys_addr < PSIO_EXP1_BASE + PSIO_EXP1_SIZE) {
        if (!psio.firmware_rom) {
            return 0xFFFFFFFF; // unmapped ROM reads as all-ones
        }

        return le_load(psio.firmware_rom, phys_addr - PSIO_EXP1_BASE, size);
    }

    return 0;
}

void
psio_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)size; // command and data words are 32-bit

    switch (phys_addr) {
    case PSIO_REG_DATA: // push one word of the command packet
        if (psio.tx_count < 3) {
            psio.tx_words[psio.tx_count++] = value;
        }

        if (psio.tx_count >= 3) {
            psio.status_reg &= ~PSIO_STAT_TX_FREE; // packet full, awaiting strobe
        }
        break;

    case PSIO_REG_CMD: // strobe: run the assembled packet
        process_psio_command();
        break;

    case PSIO_REG_IRQ:
        // Acknowledge/interrupt latch. Kept as a plain store for now: the
        // streaming FIFO manages RX_READY itself, so the ack has no side effect
        // until MENU.SYS pins down the real interrupt semantics.
        psio.irq_reg = value;
        break;

    default:
        // Firmware ROM window and unknown registers ignore writes.
        break;
    }
}
