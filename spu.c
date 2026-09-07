#include "spu.h"
#include "lebytes.h"

// Register offsets within the block.
#define SPUCNT_OFFSET  0x1AA // 0x1F801DAA, SPU Control Register
#define SPUSTAT_OFFSET 0x1AE // 0x1F801DAE, SPU Status Register (read-only)

static uint8_t spu_regs[SPU_SIZE];

void
psx_spu_reset(void)
{
    for (int i = 0; i < SPU_SIZE; i++) {
        spu_regs[i] = 0;
    }
}

uint32_t
psx_spu_read(uint32_t phys_addr, int size)
{
    uint32_t off = phys_addr - SPU_BASE;

    if (off + (uint32_t)size > SPU_SIZE) {
        return 0;
    }

    // SPUSTAT mirrors the low 6 bits of SPUCNT (the current transfer mode), so
    // the BIOS sees the mode it just requested. The rest reads back as written.
    if (off == SPUSTAT_OFFSET) {
        return load_sized(spu_regs, SPUCNT_OFFSET, 2) & 0x3F;
    }

    return load_sized(spu_regs, off, size);
}

void
psx_spu_write(uint32_t phys_addr, uint32_t value, int size)
{
    uint32_t off = phys_addr - SPU_BASE;

    if (off + (uint32_t)size > SPU_SIZE) {
        return;
    }

    store_sized(spu_regs, off, value, size);
}

void
spu_cd_audio(const int16_t *frames, int nframes)
{
    // No audio backend yet: the SPU is a register stub, so the CD-DA stream is
    // discarded. When the SPU can emit sound, mix these frames in here scaled by
    // the CD volume registers (0x1F801DB0). Kept as the CD-audio input seam.
    (void)frames;
    (void)nframes;
}
