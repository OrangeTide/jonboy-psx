#ifndef PSIO_H_
#define PSIO_H_
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    FILE *sd_card_file;        // Handle to the user's virtual SD card (.img/.bin)
    uint8_t *firmware_rom;     // 512KB allocated buffer holding the PSIO menu/ARM firmware

    // Virtual Registers
    uint32_t status_reg;       // Flags: Ready, Data, Error (see PSIO_STATUS_*)
    uint32_t command_reg;      // Current command sent by PSX CPU (PSIO_CMD_*)
    uint32_t lba;              // Sector address for the next read

    // FIFO Buffer for streaming data
    uint8_t fifo_buffer[4096];
    uint32_t fifo_ptr;
    uint32_t fifo_count;
} PsioDevice;

// The image is addressed in fixed-size sectors. This matches a CD-ROM
// Mode1/Mode2-Form1 user-data sector, which is what a booted game reads.
#define PSIO_SECTOR_SIZE 2048

// Status register bit flags (read at PSIO_REG_STATUS).
#define PSIO_STATUS_READY 0x01u // last command finished / device idle
#define PSIO_STATUS_DATA  0x02u // FIFO holds unread bytes
#define PSIO_STATUS_ERROR 0x04u // last command failed (no image, seek/read error)

// Command opcodes (written to PSIO_REG_STATUS). Unknown values are treated as a
// no-op that still completes, so a probe write cannot stall the guest.
#define PSIO_CMD_NOP         0x00u
#define PSIO_CMD_READ_SECTOR 0x01u // read the sector at the current LBA into the FIFO

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
bool load_psio_firmware(const char *filepath);
void process_psio_command(void);

// Physical-address accessors for the two PSIO windows, called by the memory
// decoder. size is 1, 2, or 4 bytes.
uint32_t psio_read(uint32_t phys_addr, int size);
void psio_write(uint32_t phys_addr, uint32_t value, int size);

#endif
