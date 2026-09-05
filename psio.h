#ifndef PSIO_H_
#define PSIO_H_
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    FILE *sd_card_file;        // Handle to the user's virtual SD card (.img/.bin)
    uint8_t *firmware_rom;     // 512KB allocated buffer holding the PSIO menu/ARM firmware

    // Virtual Registers
    uint32_t status_reg;       // Flags: Ready, Busy, Error, etc.
    uint32_t command_reg;      // Current command sent by PSX CPU

    // FIFO Buffer for streaming data
    uint8_t fifo_buffer[4096];
    uint32_t fifo_ptr;
    uint32_t fifo_count;
} PsioDevice;

// PSIO occupies two windows in the PSX expansion space.
#define PSIO_EXP1_BASE 0x1F000000 // firmware ROM (Expansion Region 1)
#define PSIO_EXP1_SIZE 0x00080000 // 512 KB
#define PSIO_REG_BASE  0x1F802000 // device registers (Expansion Region 2)
#define PSIO_REG_SIZE  0x00000010

// Register offsets within the device window.
#define PSIO_REG_STATUS (PSIO_REG_BASE + 0x00) // read: status, write: command
#define PSIO_REG_FIFO   (PSIO_REG_BASE + 0x04) // read: pop SD data FIFO
#define PSIO_REG_LBA    (PSIO_REG_BASE + 0x08) // write: sector address

extern PsioDevice psio;
bool load_virtual_sd_card(const char *filepath);
void process_psio_command(void);

// Physical-address accessors for the two PSIO windows, called by the memory
// decoder. size is 1, 2, or 4 bytes.
uint32_t psio_read(uint32_t phys_addr, int size);
void psio_write(uint32_t phys_addr, uint32_t value, int size);

#endif
