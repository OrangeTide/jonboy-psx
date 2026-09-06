#ifndef CDROM_H_
#define CDROM_H_
#include <stdint.h>

// The CD-ROM controller register block (0x1F801800 - 0x1F801803). This is a
// bare-minimum stub: it answers commands well enough for the BIOS shell to
// finish probing the drive, and it always reports the disc as not present, so
// the shell falls through to its menu instead of trying to boot a disc.
#define CDROM_BASE 0x1F801800
#define CDROM_SIZE 4

uint32_t psx_cdrom_read(uint32_t phys_addr, int size);
void psx_cdrom_write(uint32_t phys_addr, uint32_t value, int size);
void psx_cdrom_reset(void);
void psx_cdrom_tick(uint32_t cycles);

#endif
