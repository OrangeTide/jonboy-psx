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

extern PsioDevice psio;
bool load_virtual_sd_card(const char *filepath);
void process_psio_command(void);

#endif
