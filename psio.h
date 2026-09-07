#ifndef PSIO_H_
#define PSIO_H_
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    FILE *sd_card_file;        // Handle to the user's virtual SD card (.img/.bin)
    uint8_t *firmware_rom;     // EXP1 ROM image (PSIO loader / MENU boot ROM)
} PsioDevice;

// The PSIO cartridge lives on the parallel-port bus in Expansion Region 1. To
// the PS1 the window is a ROM: the BIOS pre/post boot check looks here for a
// signature and, if it matches, jumps to the vector next to it. That is how the
// cartridge takes control and runs its loader.
#define PSIO_EXP1_BASE 0x1F000000
#define PSIO_EXP1_SIZE 0x00080000 // 512 KB

// EXP1 boot-ROM header layout the BIOS checks. The pre-boot hook runs before
// the kernel is set up; the post-boot hook runs after. Each is a jump/entry at
// the vector offset followed by the licence signature the BIOS matches.
#define PSIO_POST_VECTOR (PSIO_EXP1_BASE + 0x00)
#define PSIO_POST_SIG    (PSIO_EXP1_BASE + 0x04)
#define PSIO_PRE_VECTOR  (PSIO_EXP1_BASE + 0x80)
#define PSIO_PRE_SIG     (PSIO_EXP1_BASE + 0x84)
#define PSIO_BOOT_SIGNATURE "Licensed by Sony Computer Entertainment Inc."

extern PsioDevice psio;
bool load_virtual_sd_card(const char *filepath);
bool load_psio_firmware(const char *filepath);

// Physical-address accessors for the EXP1 window, called by the memory decoder.
// size is 1, 2, or 4 bytes.
uint32_t psio_read(uint32_t phys_addr, int size);
void psio_write(uint32_t phys_addr, uint32_t value, int size);

#endif
