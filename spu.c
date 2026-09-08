#include "spu.h"
#include "cpu.h"
#include "lebytes.h"

#include <stdatomic.h>
#include <string.h>

// SPU with 24-voice ADPCM synthesis. Each voice reads 16-byte ADPCM blocks from
// the 512 KB sound RAM, decodes them, resamples by its pitch, shapes them with an
// ADSR envelope, and mixes into the output with its per-voice and the main
// volume. Reverb, noise voices, pitch modulation, and per-voice/main volume
// sweeps are modeled. CD-DA and XA audio is mixed in through spu_cd_audio, scaled
// by the CD volume; when the SPUCNT CD-reverb bit is set the CD audio is also
// routed through the reverb work area. The mixed stream is produced at 44.1 kHz
// and queued for the app to play with spu_output_pull.
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
#define IRQ_ADDR_OFFSET   0x1A4 // sound RAM IRQ address (8-byte units)
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
static int spu_irq_flag;         // sound-RAM IRQ (interrupt 9) latched, SPUSTAT.6

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

// A volume that can be a fixed level or a sweep envelope. The sweep advances one
// step per output sample using the same rate tables as the ADSR.
typedef struct {
    int level;   // current level, -0x8000..0x7FFF
    int counter; // fine counter
} Sweep;

static Sweep voice_sweep[NUM_VOICES][2]; // [voice][0=L,1=R]
static Sweep main_sweep[2];

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
// drained by spu_output_pull. This is a single-producer/single-consumer ring:
// the emulation thread writes (mix_frame) and, in the windowed build, the SDL
// audio callback thread reads. The two index counters are atomic with
// release/acquire ordering so the reader sees a frame's data before it sees the
// advanced write index, and the writer sees space the reader has freed. The ring
// size is a power of two so the free-running counters mask straight to an index.
#define SPU_RING_FRAMES 8192
static int16_t spu_ring[SPU_RING_FRAMES * 2];
static _Atomic unsigned spu_ring_w, spu_ring_r;

// CD audio waiting to be mixed. spu_cd_audio fills this; the mixer drains one
// frame per output frame.
#define CD_FIFO_FRAMES 8192
static int16_t cd_fifo[CD_FIFO_FRAMES * 2];
static unsigned cd_fifo_w, cd_fifo_r;

// One output sample every 768 CPU cycles gives 44.1 kHz.
#define SPU_CYCLES_PER_SAMPLE 768
static uint32_t spu_cycle_acc;

// Noise generator (Dr. Hell/Xebra model). Clocked once per output sample at a
// rate set by the SPUCNT noise-frequency field; voices with their NON bit set
// output this value instead of ADPCM.
static uint32_t spu_noise_val;
static uint32_t spu_noise_count;

static void
noise_clock(void)
{
    static const char wave[64] = {
        1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1,
        1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0,
        1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1
    };
    static const unsigned short freqadd[5] = { 0, 84, 140, 180, 210 };

    int nclk = ((int)load_sized(spu_regs, SPUCNT_OFFSET, 2) >> 8) & 0x3F;
    unsigned int level = (0x8000u >> (nclk >> 2)) << 16;

    spu_noise_count += 0x10000;
    spu_noise_count += freqadd[nclk & 3];
    if ((spu_noise_count & 0xFFFF) >= freqadd[4]) {
        spu_noise_count += 0x10000;
        spu_noise_count -= freqadd[nclk & 3];
    }

    if (spu_noise_count >= level) {
        while (spu_noise_count >= level) {
            spu_noise_count -= level;
        }
        spu_noise_val = (spu_noise_val << 1) | wave[(spu_noise_val >> 10) & 63];
    }
}

#define PMON_OFFSET    0x190 // voice pitch-modulation bits
#define NON_OFFSET     0x194 // voice noise-mode bits
#define EON_OFFSET     0x198 // voice reverb-enable bits
#define VLOUT_OFFSET   0x184 // reverb output volume left
#define VROUT_OFFSET   0x186 // reverb output volume right
#define MBASE_OFFSET   0x1A2 // reverb work area start (8-byte units)
#define REVERB_OFFSET  0x1C0 // reverb coefficient/address block

// Reverb state, following Neill Corlett's model as implemented in PEOPS
// (REF/pcsx-redux/src/spu/reverb.cc). Addresses are 16-bit-word offsets into
// sound RAM; StartAddr/CurrAddr roll through the work area at 22.05 kHz.
static struct {
    int StartAddr, CurrAddr;
    int VolLeft, VolRight;
    int FB_SRC_A, FB_SRC_B;
    int IIR_ALPHA, IIR_COEF;
    int ACC_COEF_A, ACC_COEF_B, ACC_COEF_C, ACC_COEF_D;
    int FB_ALPHA, FB_X;
    int IIR_DEST_A0, IIR_DEST_A1, IIR_DEST_B0, IIR_DEST_B1;
    int ACC_SRC_A0, ACC_SRC_A1, ACC_SRC_B0, ACC_SRC_B1;
    int ACC_SRC_C0, ACC_SRC_C1, ACC_SRC_D0, ACC_SRC_D1;
    int IIR_SRC_A0, IIR_SRC_A1, IIR_SRC_B0, IIR_SRC_B1;
    int MIX_DEST_A0, MIX_DEST_A1, MIX_DEST_B0, MIX_DEST_B1;
    int IN_COEF_L, IN_COEF_R;
    int iLastL, iLastR, iL, iR;
    int cnt;
} rvb;

void
psx_spu_reset(void)
{
    memset(spu_regs, 0, sizeof spu_regs);
    memset(spu_ram, 0, sizeof spu_ram);
    memset(voices, 0, sizeof voices);
    memset(&rvb, 0, sizeof rvb);
    memset(voice_sweep, 0, sizeof voice_sweep);
    memset(main_sweep, 0, sizeof main_sweep);
    spu_noise_val = 0;
    spu_noise_count = 0;
    spu_xfer_addr = 0;
    spu_endx = 0;
    spu_irq_flag = 0;
    atomic_store_explicit(&spu_ring_w, 0, memory_order_relaxed);
    atomic_store_explicit(&spu_ring_r, 0, memory_order_relaxed);
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

// Sound-RAM IRQ (interrupt 9). When IRQ9 is enabled in SPUCNT (bit 6) and the
// SPU touches the sound-RAM address in the IRQ register, it latches SPUSTAT bit 6
// and pulls the CPU interrupt line. addr is a byte address in sound RAM and len
// the number of bytes touched by the access (an ADPCM block read or a transfer
// word). Voices and the transfer port call this on every sound-RAM access.
static void
spu_check_irq(uint32_t addr, uint32_t len)
{
    if (!(load_sized(spu_regs, SPUCNT_OFFSET, 2) & 0x0040)) {
        return; // IRQ9 disabled
    }

    uint32_t irq = ((uint32_t)load_sized(spu_regs, IRQ_ADDR_OFFSET, 2) * 8) &
                   (SPU_RAM_SIZE - 1);
    addr &= (SPU_RAM_SIZE - 1);

    if (irq >= addr && irq < addr + len) {
        spu_irq_flag = 1;
        cpu.i_stat |= (1u << IRQ_SPU);
    }
}

// A volume register is a signed level in fixed mode; the sweep mode (bit15) is
// not modeled here, so approximate it at unity. Used for the CD and reverb
// output volumes; the voice and main volumes use vol_tick for real sweeps.
static int
vol_level(uint16_t v)
{
    return (v & 0x8000) ? 0x7FFF : (int16_t)v;
}

static int
vol_tick(uint16_t reg, Sweep *sw)
{
    if (!(reg & 0x8000)) {
        // Fixed volume: track it so a later sweep continues from here.
        sw->level = (int16_t)reg;
        sw->counter = 0;
        return sw->level;
    }

    int rate = reg & 0x7F;
    int exp = (reg >> 14) & 1;
    int dec = (reg >> 13) & 1;
    int neg = (reg >> 12) & 1; // sweep phase: 0 = positive, 1 = negative
    int idx;

    if (!dec) { // increase toward the phase extreme
        int r = rate;
        if (exp && sw->level >= 0x6000) {
            r += 8;
        }
        idx = r > 127 ? 127 : r;
        if (++sw->counter >= env_denom[idx]) {
            sw->counter = 0;
            sw->level += neg ? -env_num_inc[idx] : env_num_inc[idx];
        }
        if (!neg && sw->level > 0x7FFF) {
            sw->level = 0x7FFF;
        }
        if (neg && sw->level < -0x8000) {
            sw->level = -0x8000;
        }
    } else { // decrease toward zero
        idx = rate > 127 ? 127 : rate;
        if (++sw->counter >= env_denom[idx]) {
            sw->counter = 0;
            int mag = sw->level < 0 ? -sw->level : sw->level;
            int step = exp ? (env_num_dec[idx] * mag) >> 15 : env_num_dec[idx];
            sw->level += neg ? -step : step;
        }
        if (!neg && sw->level < 0) {
            sw->level = 0;
        }
        if (neg && sw->level > 0) {
            sw->level = 0;
        }
    }
    return sw->level;
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
    spu_check_irq(ch->cur_addr, ADPCM_BLOCK); // voice read of this sound-RAM block
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

// Produce one enveloped sample for a voice and advance its position. pmod is the
// previous voice's output sample, used when this voice has pitch modulation on.
static int
voice_next_sample(int v, int pmod)
{
    Voice *ch = &voices[v];
    uint32_t base = (uint32_t)v * 0x10;

    // Noise-mode voice: output the noise generator, still shaped by the
    // envelope. It ignores the ADPCM stream and pitch.
    if (load_sized(spu_regs, NON_OFFSET, 4) & (1u << v)) {
        env_step(ch, base);
        return ((int)(int16_t)spu_noise_val * ch->env) >> 15;
    }

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
    // Pitch modulation: scale the step by the previous voice's output.
    if (v > 0 && (load_sized(spu_regs, PMON_OFFSET, 4) & (1u << v))) {
        int factor = pmod + 0x8000; // 0..0xFFFF
        pitch = (pitch * factor) >> 15;
    }
    if (pitch > 0x3FFF) {
        pitch = 0x3FFF;
    }
    if (pitch < 0) {
        pitch = 0;
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

// Wrap a reverb offset (in 8-byte units) into the work area, returning a
// 16-bit-word index into sound RAM. The extra offset is 0 or 1 word.
static int
rev_wrap(int off, int extra)
{
    int i = off * 4 + rvb.CurrAddr + extra;
    while (i > 0x3FFFF) {
        i = rvb.StartAddr + (i - 0x40000);
    }
    while (i < rvb.StartAddr) {
        i = 0x3FFFF - (rvb.StartAddr - i);
    }
    return i & 0x3FFFF; // guard against a stray coefficient
}

static int
g_buffer(int off)
{
    return ((int16_t *)spu_ram)[rev_wrap(off, 0)];
}

static void
s_buffer(int off, int val)
{
    ((int16_t *)spu_ram)[rev_wrap(off, 0)] = clamp16(val);
}

static void
s_buffer1(int off, int val)
{
    ((int16_t *)spu_ram)[rev_wrap(off, 1)] = clamp16(val);
}

// Run one 44.1 kHz reverb step. The heavy filtering happens at 22.05 kHz (every
// second call); the off cycle holds the last value.
static void
reverb_run(int in_l, int in_r, int *out_l, int *out_r)
{
    uint16_t spucnt = (uint16_t)load_sized(spu_regs, SPUCNT_OFFSET, 2);

    if (!rvb.StartAddr || !(spucnt & 0x0080)) { // reverb master enable is bit7
        rvb.iLastL = rvb.iLastR = rvb.iL = rvb.iR = 0;
        *out_l = *out_r = 0;
        return;
    }

    rvb.cnt++;
    if (!(rvb.cnt & 1)) {
        *out_l = rvb.iL;
        *out_r = rvb.iR;
        return;
    }

    int IIR_INPUT_A0 = (g_buffer(rvb.IIR_SRC_A0) * rvb.IIR_COEF) / 32768 + (in_l * rvb.IN_COEF_L) / 32768;
    int IIR_INPUT_A1 = (g_buffer(rvb.IIR_SRC_A1) * rvb.IIR_COEF) / 32768 + (in_r * rvb.IN_COEF_R) / 32768;
    int IIR_INPUT_B0 = (g_buffer(rvb.IIR_SRC_B0) * rvb.IIR_COEF) / 32768 + (in_l * rvb.IN_COEF_L) / 32768;
    int IIR_INPUT_B1 = (g_buffer(rvb.IIR_SRC_B1) * rvb.IIR_COEF) / 32768 + (in_r * rvb.IN_COEF_R) / 32768;

    int IIR_A0 = (IIR_INPUT_A0 * rvb.IIR_ALPHA) / 32768 + (g_buffer(rvb.IIR_DEST_A0) * (32768 - rvb.IIR_ALPHA)) / 32768;
    int IIR_A1 = (IIR_INPUT_A1 * rvb.IIR_ALPHA) / 32768 + (g_buffer(rvb.IIR_DEST_A1) * (32768 - rvb.IIR_ALPHA)) / 32768;
    int IIR_B0 = (IIR_INPUT_B0 * rvb.IIR_ALPHA) / 32768 + (g_buffer(rvb.IIR_DEST_B0) * (32768 - rvb.IIR_ALPHA)) / 32768;
    int IIR_B1 = (IIR_INPUT_B1 * rvb.IIR_ALPHA) / 32768 + (g_buffer(rvb.IIR_DEST_B1) * (32768 - rvb.IIR_ALPHA)) / 32768;

    s_buffer1(rvb.IIR_DEST_A0, IIR_A0);
    s_buffer1(rvb.IIR_DEST_A1, IIR_A1);
    s_buffer1(rvb.IIR_DEST_B0, IIR_B0);
    s_buffer1(rvb.IIR_DEST_B1, IIR_B1);

    int ACC0 = (g_buffer(rvb.ACC_SRC_A0) * rvb.ACC_COEF_A) / 32768 +
               (g_buffer(rvb.ACC_SRC_B0) * rvb.ACC_COEF_B) / 32768 +
               (g_buffer(rvb.ACC_SRC_C0) * rvb.ACC_COEF_C) / 32768 +
               (g_buffer(rvb.ACC_SRC_D0) * rvb.ACC_COEF_D) / 32768;
    int ACC1 = (g_buffer(rvb.ACC_SRC_A1) * rvb.ACC_COEF_A) / 32768 +
               (g_buffer(rvb.ACC_SRC_B1) * rvb.ACC_COEF_B) / 32768 +
               (g_buffer(rvb.ACC_SRC_C1) * rvb.ACC_COEF_C) / 32768 +
               (g_buffer(rvb.ACC_SRC_D1) * rvb.ACC_COEF_D) / 32768;

    int FB_A0 = g_buffer(rvb.MIX_DEST_A0 - rvb.FB_SRC_A);
    int FB_A1 = g_buffer(rvb.MIX_DEST_A1 - rvb.FB_SRC_A);
    int FB_B0 = g_buffer(rvb.MIX_DEST_B0 - rvb.FB_SRC_B);
    int FB_B1 = g_buffer(rvb.MIX_DEST_B1 - rvb.FB_SRC_B);

    s_buffer(rvb.MIX_DEST_A0, ACC0 - (FB_A0 * rvb.FB_ALPHA) / 32768);
    s_buffer(rvb.MIX_DEST_A1, ACC1 - (FB_A1 * rvb.FB_ALPHA) / 32768);
    s_buffer(rvb.MIX_DEST_B0, (rvb.FB_ALPHA * ACC0) / 32768 -
             (FB_A0 * (int)(rvb.FB_ALPHA ^ 0xFFFF8000)) / 32768 - (FB_B0 * rvb.FB_X) / 32768);
    s_buffer(rvb.MIX_DEST_B1, (rvb.FB_ALPHA * ACC1) / 32768 -
             (FB_A1 * (int)(rvb.FB_ALPHA ^ 0xFFFF8000)) / 32768 - (FB_B1 * rvb.FB_X) / 32768);

    rvb.iL = (g_buffer(rvb.MIX_DEST_A0) + g_buffer(rvb.MIX_DEST_B0)) / 3;
    rvb.iR = (g_buffer(rvb.MIX_DEST_A1) + g_buffer(rvb.MIX_DEST_B1)) / 3;
    rvb.iL = (rvb.iL * rvb.VolLeft) / 0x4000;
    rvb.iR = (rvb.iR * rvb.VolRight) / 0x4000;

    rvb.CurrAddr++;
    if (rvb.CurrAddr > 0x3FFFF) {
        rvb.CurrAddr = rvb.StartAddr;
    }

    *out_l = rvb.iL;
    *out_r = rvb.iR;
}

static void
mix_frame(void)
{
    int mainl = vol_tick((uint16_t)load_sized(spu_regs, MAINVOL_L_OFFSET, 2), &main_sweep[0]);
    int mainr = vol_tick((uint16_t)load_sized(spu_regs, MAINVOL_R_OFFSET, 2), &main_sweep[1]);
    uint32_t eon = (uint32_t)load_sized(spu_regs, EON_OFFSET, 4);
    int32_t l = 0, r = 0;
    int32_t rev_in_l = 0, rev_in_r = 0;

    noise_clock();

    int prev = 0; // previous voice's output, for pitch modulation
    for (int v = 0; v < NUM_VOICES; v++) {
        int s = 0;
        if (voices[v].active) {
            uint32_t base = (uint32_t)v * 0x10;
            s = voice_next_sample(v, prev);
            int vl = vol_tick((uint16_t)load_sized(spu_regs, base + V_VOL_L, 2), &voice_sweep[v][0]);
            int vr = vol_tick((uint16_t)load_sized(spu_regs, base + V_VOL_R, 2), &voice_sweep[v][1]);
            int sl = (s * vl) >> 15;
            int sr = (s * vr) >> 15;
            l += sl;
            r += sr;
            if (eon & (1u << v)) {
                rev_in_l += sl;
                rev_in_r += sr;
            }
        }
        prev = s; // 0 for an inactive voice
    }

    // Pull one CD-audio frame (already scaled by CD volume). When the SPUCNT
    // CD-reverb bit (0x04) is set the CD audio also feeds the reverb input, so
    // it must be gathered before reverb_run.
    int cdl = 0, cdr = 0;
    if (cd_fifo_r != cd_fifo_w) {
        unsigned i = (cd_fifo_r & (CD_FIFO_FRAMES - 1)) * 2;
        cdl = cd_fifo[i];
        cdr = cd_fifo[i + 1];
        cd_fifo_r++;
    }
    if (load_sized(spu_regs, SPUCNT_OFFSET, 2) & 0x0004) {
        rev_in_l += cdl;
        rev_in_r += cdr;
    }

    l = (l * mainl) >> 15;
    r = (r * mainr) >> 15;

    // Reverb: feed the reverb-enabled voices, and the CD audio when routed,
    // through the work area and add its wet output to the mix.
    int rev_l, rev_r;
    reverb_run(rev_in_l, rev_in_r, &rev_l, &rev_r);
    l += rev_l;
    r += rev_r;

    // CD audio dry mix, at main volume (matching the voice dry path).
    l += (cdl * mainl) >> 15;
    r += (cdr * mainr) >> 15;

    unsigned wr = atomic_load_explicit(&spu_ring_w, memory_order_relaxed);
    unsigned rd = atomic_load_explicit(&spu_ring_r, memory_order_acquire);
    if (wr - rd < SPU_RING_FRAMES) {
        unsigned i = (wr & (SPU_RING_FRAMES - 1)) * 2;
        spu_ring[i] = clamp16(l);
        spu_ring[i + 1] = clamp16(r);
        atomic_store_explicit(&spu_ring_w, wr + 1, memory_order_release);
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

    // SPUSTAT mirrors the low 6 bits of SPUCNT (the current transfer mode) and
    // reports the sound-RAM IRQ flag in bit 6.
    if (off == SPUSTAT_OFFSET) {
        return (load_sized(spu_regs, SPUCNT_OFFSET, 2) & 0x3F) |
               (spu_irq_flag ? 0x40 : 0);
    }
    // ENDX reports which voices have reached a loop-end block.
    if (off == ENDX_OFFSET || off == ENDX_OFFSET + 2) {
        return (spu_endx >> ((off - ENDX_OFFSET) * 8)) & ((size == 4) ? 0xFFFFFFFF : 0xFFFF);
    }

    return load_sized(spu_regs, off, size);
}

// Assign one reverb coefficient/address register by its index within the block
// (offset (reg - REVERB_OFFSET) / 2). Mirrors the mapping in pcsx-redux.
static void
set_reverb_reg(int n, int val)
{
    int16_t v = (int16_t)val;

    switch (n) {
    case 0:  rvb.FB_SRC_A = val; break;
    case 1:  rvb.FB_SRC_B = v; break;
    case 2:  rvb.IIR_ALPHA = v; break;
    case 3:  rvb.ACC_COEF_A = v; break;
    case 4:  rvb.ACC_COEF_B = v; break;
    case 5:  rvb.ACC_COEF_C = v; break;
    case 6:  rvb.ACC_COEF_D = v; break;
    case 7:  rvb.IIR_COEF = v; break;
    case 8:  rvb.FB_ALPHA = v; break;
    case 9:  rvb.FB_X = v; break;
    case 10: rvb.IIR_DEST_A0 = v; break;
    case 11: rvb.IIR_DEST_A1 = v; break;
    case 12: rvb.ACC_SRC_A0 = v; break;
    case 13: rvb.ACC_SRC_A1 = v; break;
    case 14: rvb.ACC_SRC_B0 = v; break;
    case 15: rvb.ACC_SRC_B1 = v; break;
    case 16: rvb.IIR_SRC_A0 = v; break;
    case 17: rvb.IIR_SRC_A1 = v; break;
    case 18: rvb.IIR_DEST_B0 = v; break;
    case 19: rvb.IIR_DEST_B1 = v; break;
    case 20: rvb.ACC_SRC_C0 = v; break;
    case 21: rvb.ACC_SRC_C1 = v; break;
    case 22: rvb.ACC_SRC_D0 = v; break;
    case 23: rvb.ACC_SRC_D1 = v; break;
    case 24: rvb.IIR_SRC_B1 = v; break; // note: +48 is B1, +50 is B0
    case 25: rvb.IIR_SRC_B0 = v; break;
    case 26: rvb.MIX_DEST_A0 = v; break;
    case 27: rvb.MIX_DEST_A1 = v; break;
    case 28: rvb.MIX_DEST_B0 = v; break;
    case 29: rvb.MIX_DEST_B1 = v; break;
    case 30: rvb.IN_COEF_L = v; break;
    case 31: rvb.IN_COEF_R = v; break;
    default: break;
    }
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

    if (off == SPUCNT_OFFSET) {
        // Writing SPUCNT with the IRQ9-enable bit clear acknowledges (resets) the
        // latched sound-RAM IRQ flag. Games clear then re-set the bit to ack.
        if (!(value & 0x0040)) {
            spu_irq_flag = 0;
        }
        return;
    }
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
    } else if (off == VLOUT_OFFSET) {
        rvb.VolLeft = (int16_t)value;
    } else if (off == VROUT_OFFSET) {
        rvb.VolRight = (int16_t)value;
    } else if (off == MBASE_OFFSET) {
        rvb.StartAddr = ((int)(value & 0xFFFF)) << 2;
        rvb.CurrAddr = rvb.StartAddr;
    } else if (off >= REVERB_OFFSET && off < REVERB_OFFSET + 64) {
        set_reverb_reg((off - REVERB_OFFSET) / 2, (int)value);
    }
}

void
spu_transfer_write(uint16_t v)
{
    spu_check_irq(spu_xfer_addr, 2);
    store_sized(spu_ram, spu_xfer_addr & (SPU_RAM_SIZE - 1), v, 2);
    spu_xfer_addr = (spu_xfer_addr + 2) & (SPU_RAM_SIZE - 1);
}

uint16_t
spu_transfer_read(void)
{
    spu_check_irq(spu_xfer_addr, 2);
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

    // Scale by the CD volume only. The mixer applies the main volume to the dry
    // path and, when the CD-reverb bit is set, routes this CD-volume-level audio
    // through the reverb work area.
    for (int i = 0; i < nframes; i++) {
        int l = 0, r = 0;
        if (cd_enable) {
            l = ((int)frames[i * 2] * vl) >> 15;
            r = ((int)frames[i * 2 + 1] * vr) >> 15;
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
    unsigned r = atomic_load_explicit(&spu_ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&spu_ring_w, memory_order_acquire);

    while (n < max_frames && r != w) {
        unsigned i = (r & (SPU_RING_FRAMES - 1)) * 2;
        dst[n * 2] = spu_ring[i];
        dst[n * 2 + 1] = spu_ring[i + 1];
        r++;
        n++;
    }
    atomic_store_explicit(&spu_ring_r, r, memory_order_release);
    return n;
}
