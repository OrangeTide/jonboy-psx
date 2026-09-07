#ifndef SPU_H_
#define SPU_H_
#include <stdint.h>

// The SPU register block (0x1F801C00 - 0x1F801FFF). This is only a register
// stub: it stores writes and reads them back, which is enough for the BIOS SPU
// init handshake. No audio is generated.
#define SPU_BASE 0x1F801C00
#define SPU_SIZE 0x400

uint32_t psx_spu_read(uint32_t phys_addr, int size);
void psx_spu_write(uint32_t phys_addr, uint32_t value, int size);
void psx_spu_reset(void);

// CD audio input. The CD-ROM controller pushes decoded CD-DA (and, later, XA)
// PCM here as interleaved 16-bit stereo frames. The SPU mixes this into its
// output, scaled by the CD volume registers. No audio backend exists yet, so
// this currently discards; it is the seam for CD music once the SPU emits sound.
void spu_cd_audio(const int16_t *frames, int nframes);

#endif
