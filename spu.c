#include "spu.h"
#include "lebytes.h"

#include <string.h>

// SPU with 24-voice ADPCM synthesis. Each voice reads 16-byte ADPCM blocks from
// the 512 KB sound RAM, decodes them, resamples by its pitch, shapes them with an
// ADSR envelope, and mixes into the output with its per-voice and the main
// volume. CD (and later XA) audio is mixed in through spu_cd_audio. The mixed
// stream is produced at 44.1 kHz and queued for the app to play with
// spu_output_pull. Not modeled yet: reverb, noise voices, pitch modulation, and
// volume sweeps (fixed volume levels only).
//
// The ADSR envelope uses the PEOPS/Pete Bernert coarse-plus-fine counter model
// (see REF/pcsx-redux/src/spu/adsr.cc) with three rate tables.

// Register offsets within the block (relative to SPU_BASE 0x1F801C00). Voice n
// occupies 0x10 bytes at n*0x10.
#define V_VOL_L   0x0
#define V_VOL_R   0x2
#define V_PITCH   0x4
#define V_START   0x6
#define V_ADSR1   0x8
#define V_ADSR2   0xA
#define V_ADSRVOL 0xC
#define V_REPEAT  0xE

#define MAINVOL_L_OFFSET  0x180
#define MAINVOL_R_OFFSET  0x182
#define KON_OFFSET        0x188 // Key On  (32-bit, voices 0..23)
#define KOFF_OFFSET       0x18C // Key Off (32-bit)
#define ENDX_OFFSET       0x19C // voice end flags (read)
#define TRANSFER_ADDR_OFF 0x1A6 // sound RAM transfer address (8-byte units)
#define TRANSFER_FIFO_OFF 0x1A8 // sound RAM transfer FIFO (PIO write port)
#define SPUCNT_OFFSET     0x1AA
#define SPUSTAT_OFFSET    0x1AE
#define CDVOL_L_OFFSET    0x1B0
#define CDVOL_R_OFFSET    0x1B2

#define NUM_VOICES 24
#define SPU_RAM_SIZE (512 * 1024)
#define ADPCM_BLOCK 16   // bytes per ADPCM block
#define ADPCM_SAMPLES 28 // samples decoded per block

static uint8_t spu_regs[SPU_SIZE];
static uint8_t spu_ram[SPU_RAM_SIZE];
static uint32_t spu_xfer_addr;   // current sound-RAM transfer pointer (bytes)
static uint32_t spu_endx;        // voice end flags

typedef enum { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE } EnvState;

typedef struct {
    int active;
    uint32_t cur_addr;    // current ADPCM block address in sound RAM (bytes)
    uint32_t repeat_addr; // loop target
    int block_pos;        // 0..27 within the decoded block
    int pitch_counter;    // fractional sample position, 0x1000 = one sample
    int16_t decoded[ADPCM_SAMPLES];
    int16_t hist1, hist2; // ADPCM filter history
    uint8_t cur_flags;    // loop flags of the current block
    int has_block;

    EnvState state;
    int32_t env;          // envelope level 0..32767
    int32_t envf;         // fine counter
} Voice;

static Voice voices[NUM_VOICES];

// ADSR rate tables, generated once. The step values double every four rates.
static int32_t env_denom[128];
static int32_t env_num_inc[128];
static int32_t env_num_dec[128];

static void
build_env_tables(void)
{
    for (int r = 0; r < 128; r++) {
        if (r < 48) {
            env_denom[r] = 1;
            env_num_inc[r] = (7 - (r & 3)) << (11 - (r >> 2));
            env_num_dec[r] = (-8 + (r & 3)) << (11 - (r >> 2));
        } else {
            env_denom[r] = 1 << ((r >> 2) - 11);
            env_num_inc[r] = 7 - (r & 3);
            env_num_dec[r] = -8 + (r & 3);
        }
    }
}

// Output ring of interleaved 16-bit stereo frames, produced by the mixer and
// drained by the app. Same thread on both ends, so no locking is needed.
#define SPU_RING_FRAMES 8192
static int16_t spu_ring[SPU_RING_FRAMES * 2];
static unsigned spu_ring_w, spu_ring_r;

// CD audio waiting to be mixed. spu_cd_audio fills this; the mixer drains one
// frame per output frame.
#define CD_FIFO_FRAMES 8192
static int16_t cd_fifo[CD_FIFO_FRAMES * 2];
static unsigned cd_fifo_w, cd_fifo_r;

// One output sample every 768 CPU cycles gives 44.1 kHz.
#define SPU_CYCLES_PER_SAMPLE 768
static uint32_t spu_cycle_acc;

void
psx_spu_reset(void)
{
    memset(spu_regs, 0, sizeof spu_regs);
    memset(spu_ram, 0, sizeof spu_ram);
    memset(voices, 0, sizeof voices);
    spu_xfer_addr = 0;
    spu_endx = 0;
    spu_ring_w = spu_ring_r = 0;
    cd_fifo_w = cd_fifo_r = 0;
    spu_cycle_acc = 0;
    build_env_tables();
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
key_on(int v)
{
    Voice *ch = &voices[v];
    uint32_t base = (uint32_t)v * 0x10;
    uint32_t start = (uint32_t)load_sized(spu_regs, base + V_START, 2) * 8;

    ch->active = 1;
    ch->cur_addr = start & (SPU_RAM_SIZE - 1);
    ch->repeat_addr = ch->cur_addr;
    ch->block_pos = 0;
    ch->pitch_counter = 0;
    ch->hist1 = ch->hist2 = 0;
    ch->has_block = 0;
    ch->state = ENV_ATTACK;
    ch->env = 0;
    ch->envf = 0;
    spu_endx &= ~(1u << v);
}

static void
key_off(int v)
{
    if (voices[v].active) {
        voices[v].state = ENV_RELEASE;
    }
}

static const int adpcm_filter_pos[5] = { 0, 60, 115, 98, 122 };
static const int adpcm_filter_neg[5] = { 0, 0, -52, -55, -60 };

static void
decode_block(Voice *ch)
{
    const uint8_t *b = &spu_ram[ch->cur_addr & (SPU_RAM_SIZE - 1)];
    int shift = b[0] & 0x0F;
    int filter = (b[0] >> 4) & 0x07;

    if (shift > 12) {
        shift = 9; // invalid shift behaves like 9 on hardware
    }
    if (filter > 4) {
        filter = 4;
    }

    ch->cur_flags = b[1];
    if (ch->cur_flags & 0x04) {
        ch->repeat_addr = ch->cur_addr; // loop-start marker
    }

    for (int i = 0; i < ADPCM_SAMPLES; i++) {
        int nibble = (b[2 + (i >> 1)] >> ((i & 1) * 4)) & 0x0F;
        int32_t s = (int16_t)(nibble << 12);
        s >>= shift;
        s += (ch->hist1 * adpcm_filter_pos[filter] +
              ch->hist2 * adpcm_filter_neg[filter]) >> 6;
        int16_t clamped = clamp16(s);
        ch->decoded[i] = clamped;
        ch->hist2 = ch->hist1;
        ch->hist1 = clamped;
    }
    ch->has_block = 1;
}

// Move to the next block, honoring the finished block's loop flags.
static void
advance_block(Voice *ch, int v)
{
    if (ch->cur_flags & 0x01) {
        // Loop end.
        spu_endx |= (1u << v);
        ch->cur_addr = ch->repeat_addr;
        if (!(ch->cur_flags & 0x02)) {
            // End without repeat: release the note.
            ch->state = ENV_RELEASE;
            ch->env = 0;
        }
    } else {
        ch->cur_addr = (ch->cur_addr + ADPCM_BLOCK) & (SPU_RAM_SIZE - 1);
    }
    decode_block(ch);
}

static void
env_step(Voice *ch, uint32_t base)
{
    int adsr1 = (int)load_sized(spu_regs, base + V_ADSR1, 2);
    int adsr2 = (int)load_sized(spu_regs, base + V_ADSR2, 2);
    int rate, idx;

    switch (ch->state) {
    case ENV_ATTACK: {
        int ar = (adsr1 >> 8) & 0x7F;
        int am = (adsr1 >> 15) & 1;
        rate = ar;
        if (am && ch->env >= 0x6000) {
            rate += 8;
        }
        idx = rate > 127 ? 127 : rate;
        if (++ch->envf >= env_denom[idx]) {
            ch->envf = 0;
            ch->env += env_num_inc[idx];
        }
        if (ch->env >= 32767) {
            ch->env = 32767;
            ch->state = ENV_DECAY;
        }
        break;
    }
    case ENV_DECAY: {
        int dr = (adsr1 >> 4) & 0x0F;
        int sl = adsr1 & 0x0F;
        idx = (dr * 4) > 127 ? 127 : dr * 4;
        if (++ch->envf >= env_denom[idx]) {
            ch->envf = 0;
            ch->env += (env_num_dec[idx] * ch->env) >> 15; // always exponential
        }
        if (ch->env < 0) {
            ch->env = 0;
        }
        if (((ch->env >> 11) & 0x0F) <= sl) {
            ch->state = ENV_SUSTAIN;
        }
        break;
    }
    case ENV_SUSTAIN: {
        int sm = (adsr2 >> 15) & 1;
        int sd = (adsr2 >> 14) & 1; // 1 = decrease
        int sr = (adsr2 >> 6) & 0x7F;
        rate = sr;
        if (!sd) { // increase
            if (sm && ch->env >= 0x6000) {
                rate += 8;
            }
            idx = rate > 127 ? 127 : rate;
            if (++ch->envf >= env_denom[idx]) {
                ch->envf = 0;
                ch->env += env_num_inc[idx];
            }
            if (ch->env > 32767) {
                ch->env = 32767;
            }
        } else { // decrease
            idx = rate > 127 ? 127 : rate;
            if (++ch->envf >= env_denom[idx]) {
                ch->envf = 0;
                if (sm) {
                    ch->env += (env_num_dec[idx] * ch->env) >> 15;
                } else {
                    ch->env += env_num_dec[idx];
                }
            }
            if (ch->env < 0) {
                ch->env = 0;
            }
        }
        break;
    }
    case ENV_RELEASE: {
        int rm = (adsr2 >> 5) & 1;
        int rr = adsr2 & 0x1F;
        idx = (rr * 4) > 127 ? 127 : rr * 4;
        if (++ch->envf >= env_denom[idx]) {
            ch->envf = 0;
            if (rm) {
                ch->env += (env_num_dec[idx] * ch->env) >> 15;
            } else {
                ch->env += env_num_dec[idx];
            }
        }
        if (ch->env <= 0) {
            ch->env = 0;
            ch->state = ENV_OFF;
            ch->active = 0;
        }
        break;
    }
    default:
        break;
    }
}

// Produce one enveloped sample for a voice and advance its position.
static int
voice_next_sample(int v)
{
    Voice *ch = &voices[v];
    uint32_t base = (uint32_t)v * 0x10;

    if (!ch->has_block) {
        decode_block(ch);
    }

    int frac = ch->pitch_counter & 0x0FFF;
    int s0 = ch->decoded[ch->block_pos];
    int s1 = (ch->block_pos + 1 < ADPCM_SAMPLES) ? ch->decoded[ch->block_pos + 1] : s0;
    int s = s0 + (((s1 - s0) * frac) >> 12);

    env_step(ch, base);
    s = (s * ch->env) >> 15;

    int pitch = (int)load_sized(spu_regs, base + V_PITCH, 2) & 0xFFFF;
    if (pitch > 0x3FFF) {
        pitch = 0x3FFF;
    }
    ch->pitch_counter += pitch;
    while (ch->pitch_counter >= 0x1000) {
        ch->pitch_counter -= 0x1000;
        ch->block_pos++;
        if (ch->block_pos >= ADPCM_SAMPLES) {
            ch->block_pos -= ADPCM_SAMPLES;
            advance_block(ch, v);
        }
    }
    return s;
}

static void
mix_frame(void)
{
    int mainl = vol_level((uint16_t)load_sized(spu_regs, MAINVOL_L_OFFSET, 2));
    int mainr = vol_level((uint16_t)load_sized(spu_regs, MAINVOL_R_OFFSET, 2));
    int32_t l = 0, r = 0;

    for (int v = 0; v < NUM_VOICES; v++) {
        if (!voices[v].active) {
            continue;
        }
        uint32_t base = (uint32_t)v * 0x10;
        int s = voice_next_sample(v);
        int vl = vol_level((uint16_t)load_sized(spu_regs, base + V_VOL_L, 2));
        int vr = vol_level((uint16_t)load_sized(spu_regs, base + V_VOL_R, 2));
        l += (s * vl) >> 15;
        r += (s * vr) >> 15;
    }

    l = (l * mainl) >> 15;
    r = (r * mainr) >> 15;

    // Mix in one CD-audio frame (already scaled by CD and main volume).
    if (cd_fifo_r != cd_fifo_w) {
        unsigned i = (cd_fifo_r & (CD_FIFO_FRAMES - 1)) * 2;
        l += cd_fifo[i];
        r += cd_fifo[i + 1];
        cd_fifo_r++;
    }

    if (spu_ring_w - spu_ring_r < SPU_RING_FRAMES) {
        unsigned i = (spu_ring_w & (SPU_RING_FRAMES - 1)) * 2;
        spu_ring[i] = clamp16(l);
        spu_ring[i + 1] = clamp16(r);
        spu_ring_w++;
    }
}

void
psx_spu_tick(uint32_t cycles)
{
    spu_cycle_acc += cycles;
    while (spu_cycle_acc >= SPU_CYCLES_PER_SAMPLE) {
        spu_cycle_acc -= SPU_CYCLES_PER_SAMPLE;
        mix_frame();
    }
}

uint32_t
psx_spu_read(uint32_t phys_addr, int size)
{
    uint32_t off = phys_addr - SPU_BASE;

    if (off + (uint32_t)size > SPU_SIZE) {
        return 0;
    }

    // SPUSTAT mirrors the low 6 bits of SPUCNT (the current transfer mode).
    if (off == SPUSTAT_OFFSET) {
        return load_sized(spu_regs, SPUCNT_OFFSET, 2) & 0x3F;
    }
    // ENDX reports which voices have reached a loop-end block.
    if (off == ENDX_OFFSET || off == ENDX_OFFSET + 2) {
        return (spu_endx >> ((off - ENDX_OFFSET) * 8)) & ((size == 4) ? 0xFFFFFFFF : 0xFFFF);
    }

    return load_sized(spu_regs, off, size);
}

// Apply the voice key-on/off bits carried by a write to KON/KOFF.
static void
key_write(uint32_t off, uint32_t value, int size, int on)
{
    int base = (off & 0x2) ? 16 : 0; // high half addresses voices 16..23
    int nbits = size * 8;

    for (int i = 0; i < nbits; i++) {
        int v = base + i;
        if (v >= NUM_VOICES) {
            break;
        }
        if (value & (1u << i)) {
            if (on) {
                key_on(v);
            } else {
                key_off(v);
            }
        }
    }
}

void
psx_spu_write(uint32_t phys_addr, uint32_t value, int size)
{
    uint32_t off = phys_addr - SPU_BASE;

    if (off + (uint32_t)size > SPU_SIZE) {
        return;
    }

    store_sized(spu_regs, off, value, size);

    if (off == TRANSFER_ADDR_OFF) {
        spu_xfer_addr = (value & 0xFFFF) * 8;
        return;
    }
    if (off == TRANSFER_FIFO_OFF) {
        spu_transfer_write((uint16_t)value);
        return;
    }
    if (off >= KON_OFFSET && off < KON_OFFSET + 4) {
        key_write(off, value, size, 1);
    } else if (off >= KOFF_OFFSET && off < KOFF_OFFSET + 4) {
        key_write(off, value, size, 0);
    }
}

void
spu_transfer_write(uint16_t v)
{
    store_sized(spu_ram, spu_xfer_addr & (SPU_RAM_SIZE - 1), v, 2);
    spu_xfer_addr = (spu_xfer_addr + 2) & (SPU_RAM_SIZE - 1);
}

uint16_t
spu_transfer_read(void)
{
    uint16_t v = (uint16_t)load_sized(spu_ram, spu_xfer_addr & (SPU_RAM_SIZE - 1), 2);
    spu_xfer_addr = (spu_xfer_addr + 2) & (SPU_RAM_SIZE - 1);
    return v;
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
        if (cd_fifo_w - cd_fifo_r >= CD_FIFO_FRAMES) {
            break; // full: drop
        }
        unsigned idx = (cd_fifo_w & (CD_FIFO_FRAMES - 1)) * 2;
        cd_fifo[idx] = clamp16(l);
        cd_fifo[idx + 1] = clamp16(r);
        cd_fifo_w++;
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
