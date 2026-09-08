#ifndef CDROM_H_
#define CDROM_H_
#include <stdint.h>

// The CD-ROM controller register block (0x1F801800 - 0x1F801803). It answers
// the drive commands the BIOS needs to boot a disc: GetID (licensed), Setloc,
// Setmode, SeekL, ReadN/ReadS, and Pause. Read sectors are served through the
// data FIFO and DMA channel 3. With no disc mounted it reports "no disc" so the
// BIOS falls through to its shell menu.
#define CDROM_BASE 0x1F801800
#define CDROM_SIZE 4

uint32_t psx_cdrom_read(uint32_t phys_addr, int size);
void psx_cdrom_write(uint32_t phys_addr, uint32_t value, int size);
void psx_cdrom_reset(void);
void psx_cdrom_tick(uint32_t cycles);

// Mount a disc image (.bin, raw 2352-byte sectors; a .cue is accepted and its
// referenced .bin is opened). Returns 0 on success.
int cdrom_mount(const char *path);
int cdrom_has_disc(void);
void cdrom_eject(void);

// Start/stop the asynchronous sector-prefetch worker thread. The windowed build
// starts it so a slow disc image (for example one on a network mount) does not
// block input and rendering on an fread. Headless, --boot, and --exe leave it
// off, keeping reads synchronous and bit-for-bit deterministic.
void cdrom_io_start(void);
void cdrom_io_stop(void);

// Pull `words` 32-bit words of the current sector into RAM for DMA channel 3.
void cdrom_dma_read(uint32_t madr, uint32_t words);

#endif
