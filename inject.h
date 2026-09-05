#ifndef INJECT_H_
#define INJECT_H_
#include <stdint.h>

// Overwrite the start of a BIOS image with a small stub that hands control to
// the PSIO firmware. rom_buffer must hold at least the stub (see inject.c).
void inject_boot_stub(uint8_t *rom_buffer);

#endif
