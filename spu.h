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

// Advance the SPU by the given CPU cycles, producing 44.1 kHz output frames.
void psx_spu_tick(uint32_t cycles);

// Sound-RAM transfer port used by DMA channel 4 (and the PIO FIFO). Each call
// moves one 16-bit word at the current transfer address and advances it.
void spu_transfer_write(uint16_t v);
uint16_t spu_transfer_read(void);

// CD audio input. The CD-ROM controller pushes decoded CD-DA (and, later, XA)
// PCM here as interleaved 16-bit stereo frames. The SPU mixes this into its
// output, scaled by the CD volume registers. No audio backend exists yet, so
// this currently discards; it is the seam for CD music once the SPU emits sound.
void spu_cd_audio(const int16_t *frames, int nframes);

// Drain up to max_frames stereo frames of SPU output into dst (interleaved
// 16-bit L/R). Returns the number of frames written. The app calls this and
// hands the frames to the sound device.
int spu_output_pull(int16_t *dst, int max_frames);

// Cycles until the next output sample, but only while the sound-RAM IRQ is
// armed (it can fire mid-mix). Returns UINT32_MAX otherwise, so the scheduler
// batches freely when no SPU interrupt can be raised.
uint32_t spu_next_event(void);

#endif
