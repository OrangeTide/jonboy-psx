#ifndef MC_H
#define MC_H

#include <stdint.h>

// PlayStation memory card in slot 1. The card is a raw 128 KB image (the common
// .mcr/.mcd format shared by emulators and hardware card readers): 1024 frames
// of 128 bytes, 16 blocks of 8 KB, with frame 0 as the directory header. It
// answers the SIO0 protocol driven by pad.c.

#define MC_SIZE       (128 * 1024)
#define MC_FRAME_SIZE 128
#define MC_FRAMES     (MC_SIZE / MC_FRAME_SIZE)

// Mount a memory-card image. Loads the raw 128 KB file if it exists, otherwise
// creates a freshly formatted card and writes it to that path. Returns 0 on
// success. The path is remembered so writes persist to it.
int mc_mount(const char *path);

// Is a card present in slot 1?
int mc_present(void);

// Process one SIO byte of a memory-card transaction. `pos` is the byte index
// within the transaction (0 is the 0x81 addressing byte), `value` the byte the
// console sent. Returns the byte the card shifts back and sets *ack to whether
// the card pulses its acknowledge line after this byte.
uint8_t mc_transfer(int pos, uint8_t value, int *ack);

#endif
