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

    // Keep the handle open for later bulk reads. Close any prior image first.
    if (psio.sd_card_file) {
        fclose(psio.sd_card_file);
    }

    psio.sd_card_file = f;

    return true;
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
    // The EXP1 window presents the cartridge ROM. The BIOS reads the boot-hook
    // signature here and executes from the vector next to it, so instruction
    // fetches land in this same path.
    if (phys_addr >= PSIO_EXP1_BASE && phys_addr < PSIO_EXP1_BASE + PSIO_EXP1_SIZE) {
        if (!psio.firmware_rom) {
            return 0xFFFFFFFF; // no cartridge: an unmapped ROM reads as all-ones
        }

        return le_load(psio.firmware_rom, phys_addr - PSIO_EXP1_BASE, size);
    }

    return 0;
}

void
psio_write(uint32_t phys_addr, uint32_t value, int size)
{
    (void)phys_addr;
    (void)value;
    (void)size;

    // The EXP1 window is presented as ROM to the PS1. Writes (flash programming,
    // MCU command registers) are not modeled yet; ignore them.
}
