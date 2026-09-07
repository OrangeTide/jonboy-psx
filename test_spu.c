// Host-side unit test for the SPU audio engine in spu.c: CD-audio mixing/volume
// through the output mixer, and a full ADPCM voice (upload to sound RAM, key on,
// decode, envelope, per-voice and main volume). spu.c has no other dependencies,
// so it links on its own.

#include "spu.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define MAINVOL_L 0x180
#define MAINVOL_R 0x182
#define KON       0x188
#define SPUCNT    0x1AA
#define XFER_ADDR 0x1A6
#define XFER_FIFO 0x1A8
#define CDVOL_L   0x1B0
#define CDVOL_R   0x1B2

static void wr16(uint32_t off, uint16_t v) { psx_spu_write(SPU_BASE + off, v, 2); }
static void wr32(uint32_t off, uint32_t v) { psx_spu_write(SPU_BASE + off, v, 4); }

static int failures;
static void check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

// One output sample spans 768 CPU cycles.
static void tick_samples(int n) { psx_spu_tick((uint32_t)n * 768); }

int
main(void)
{
    int16_t out[4096];

    // ---- CD audio path: enabled, full volume, mixed to the output. ----
    psx_spu_reset();
    wr16(SPUCNT, 0x0001);   // CD audio enable
    wr16(CDVOL_L, 0x7FFF);
    wr16(CDVOL_R, 0x7FFF);
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);

    int16_t cd[8] = { 1000, -2000, 3000, -4000, 500, 600, -700, -800 };
    spu_cd_audio(cd, 4);      // queue 4 CD frames
    tick_samples(4);          // mixer moves them to the output ring
    int n = spu_output_pull(out, 4096);
    check("cd frame count", n, 4);
    // No voices active, so output equals the CD frames scaled by ~unity.
    int cd_ok = 1;
    for (int i = 0; i < 8; i++) {
        int want = ((int)cd[i] * 0x7FFF >> 15) * 0x7FFF >> 15;
        if (out[i] != want) {
            cd_ok = 0;
        }
    }
    if (!cd_ok) {
        fprintf(stderr, "FAIL: CD mix values wrong\n");
        failures++;
    }

    // ---- Voice path: upload one ADPCM block, key on voice 0, decode. ----
    psx_spu_reset();
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);

    // ADPCM block at sound-RAM byte 0x1000 (transfer addr is in 8-byte units).
    // header: shift 0, filter 0, flags 0x03 (loop end + repeat) so it sustains;
    // every data nibble is 1, so each decoded sample is (1<<12) = 4096.
    wr16(XFER_ADDR, 0x1000 / 8);
    wr16(XFER_FIFO, 0x0300);           // byte0=0x00 (shift/filter), byte1=0x03 (flags)
    for (int i = 0; i < 7; i++) {
        wr16(XFER_FIFO, 0x1111);       // 14 data bytes, all nibbles = 1
    }

    // Voice 0 registers.
    wr16(0x0, 0x7FFF);                  // volume L
    wr16(0x2, 0x7FFF);                  // volume R
    wr16(0x4, 0x1000);                  // pitch 1.0 (44.1 kHz)
    wr16(0x6, 0x1000 / 8);             // start address
    wr16(0x8, 0x000F);                 // ADSR1: fast attack, decay 0, sustain level 15
    wr16(0xA, 0x0000);                 // ADSR2: sustain increase, release 0

    wr32(KON, 0x00000001);             // key on voice 0

    tick_samples(64);                  // let the envelope reach full
    n = spu_output_pull(out, 4096);
    check("voice frame count", n, 64);

    int peak = 0, nonzero = 0;
    for (int i = 0; i < n * 2; i++) {
        int a = out[i] < 0 ? -out[i] : out[i];
        if (a > peak) {
            peak = a;
        }
        if (out[i] != 0) {
            nonzero++;
        }
    }
    if (!nonzero) {
        fprintf(stderr, "FAIL: voice produced silence\n");
        failures++;
    }
    // Full envelope * full volume on a 4096 sample lands just under 4096.
    if (peak < 3900 || peak > 4096) {
        fprintf(stderr, "FAIL: voice peak %d out of range (want ~4093)\n", peak);
        failures++;
    }

    if (failures) {
        fprintf(stderr, "SPU test: %d failures\n", failures);
        return 1;
    }
    printf("PASS: SPU CD mix and ADPCM voice synthesis correct (voice peak %d)\n",
           peak);
    return 0;
}
