#ifndef MC_H
#define MC_H

#include <stdint.h>

// PlayStation memory cards, one per controller port (slot 0 and slot 1). Each
// card is a raw 128 KB image (the common .mcr/.mcd format shared by emulators
// and hardware card readers): 1024 frames of 128 bytes, 16 blocks of 8 KB, with
// frame 0 as the directory header. They answer the SIO0 protocol driven by pad.c,
// which routes a transaction to a slot by the SIO port-select bit.

#define MC_SIZE       (128 * 1024)
#define MC_FRAME_SIZE 128
#define MC_FRAMES     (MC_SIZE / MC_FRAME_SIZE)

// Mount a memory-card image into a slot (0 or 1). Loads the raw 128 KB file if it
// exists, otherwise creates a freshly formatted card and writes it to that path.
// Returns 0 on success. The path is remembered so writes persist to it.
int mc_mount_slot(int slot, const char *path);
int mc_mount(const char *path); // slot 0

// Is a card present in the given slot (0 or 1)?
int mc_present_slot(int slot);
int mc_present(void); // slot 0

// Remove the card from a slot so it stops answering the SIO0 protocol. Writes
// already persist to the backing file as they happen, so nothing is lost.
void mc_eject_slot(int slot);

// Process one SIO byte of a memory-card transaction on a slot. `pos` is the byte
// index within the transaction (0 is the 0x81 addressing byte), `value` the byte
// the console sent. Returns the byte the card shifts back and sets *ack to
// whether the card pulses its acknowledge line after this byte.
uint8_t mc_transfer_slot(int slot, int pos, uint8_t value, int *ack);
uint8_t mc_transfer(int pos, uint8_t value, int *ack); // slot 0

#endif
