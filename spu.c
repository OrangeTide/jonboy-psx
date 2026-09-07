#include "spu.h"
#include "lebytes.h"

// The SPU register block is still a stub for voice synthesis: writes are stored
// and read back, which satisfies the BIOS SPU init handshake. What is real is
// the audio output path: CD-DA (and later XA) samples pushed in through
// spu_cd_audio are scaled by the CD and main volume registers and queued in a
// ring buffer, which the app drains to the sound device with spu_output_pull.
// Voice channels are not synthesized yet, so only CD audio reaches the output.

// Register offsets within the block (relative to SPU_BASE 0x1F801C00).
#define MAINVOL_L_OFFSET 0x180 // 0x1F801D80 Main Volume Left
#define MAINVOL_R_OFFSET 0x182 // 0x1F801D82 Main Volume Right
#define SPUCNT_OFFSET    0x1AA // 0x1F801DAA SPU Control Register
#define SPUSTAT_OFFSET   0x1AE // 0x1F801DAE SPU Status Register (read-only)
#define CDVOL_L_OFFSET   0x1B0 // 0x1F801DB0 CD Audio Input Volume Left
#define CDVOL_R_OFFSET   0x1B2 // 0x1F801DB2 CD Audio Input Volume Right

static uint8_t spu_regs[SPU_SIZE];

// Output ring of interleaved 16-bit stereo frames. The emulation thread pushes
// through spu_cd_audio; the app pulls with spu_output_pull. Both run on the same
// thread, so the free-running indices need no locking.
#define SPU_RING_FRAMES 8192 // power of two, ~0.19 s at 44.1 kHz
static int16_t spu_ring[SPU_RING_FRAMES * 2];
static unsigned spu_ring_w;
static unsigned spu_ring_r;

void
psx_spu_reset(void)
{
    for (int i = 0; i < SPU_SIZE; i++) {
        spu_regs[i] = 0;
    }
    spu_ring_w = 0;
    spu_ring_r = 0;
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

static int16_t
clamp16(int v)
{
    if (v < -32768) {
        return -32768;
    }
    if (v > 32767) {
        return 32767;
    }
    return (int16_t)v;
}

// A volume register is a signed level in fixed mode; the sweep mode (bit15) is
// not modeled, so approximate it at unity.
static int
vol_level(uint16_t v)
{
    return (v & 0x8000) ? 0x7FFF : (int16_t)v;
}

static void
ring_push(int l, int r)
{
    if (spu_ring_w - spu_ring_r >= SPU_RING_FRAMES) {
        return; // full: drop rather than overwrite unplayed audio
    }
    unsigned i = (spu_ring_w & (SPU_RING_FRAMES - 1)) * 2;
    spu_ring[i] = (int16_t)l;
    spu_ring[i + 1] = (int16_t)r;
    spu_ring_w++;
}

void
spu_cd_audio(const int16_t *frames, int nframes)
{
    uint16_t spucnt = (uint16_t)load_sized(spu_regs, SPUCNT_OFFSET, 2);
    int cd_enable = spucnt & 0x0001;
    int vl = vol_level((uint16_t)load_sized(spu_regs, CDVOL_L_OFFSET, 2));
    int vr = vol_level((uint16_t)load_sized(spu_regs, CDVOL_R_OFFSET, 2));
    int ml = vol_level((uint16_t)load_sized(spu_regs, MAINVOL_L_OFFSET, 2));
    int mr = vol_level((uint16_t)load_sized(spu_regs, MAINVOL_R_OFFSET, 2));

    for (int i = 0; i < nframes; i++) {
        int l = 0, r = 0;
        if (cd_enable) {
            l = ((int)frames[i * 2] * vl) >> 15;
            r = ((int)frames[i * 2 + 1] * vr) >> 15;
            l = (l * ml) >> 15;
            r = (r * mr) >> 15;
        }
        ring_push(clamp16(l), clamp16(r));
    }
}

int
spu_output_pull(int16_t *dst, int max_frames)
{
    int n = 0;

    while (n < max_frames && spu_ring_r != spu_ring_w) {
        unsigned i = (spu_ring_r & (SPU_RING_FRAMES - 1)) * 2;
        dst[n * 2] = spu_ring[i];
        dst[n * 2 + 1] = spu_ring[i + 1];
        spu_ring_r++;
        n++;
    }
    return n;
}
