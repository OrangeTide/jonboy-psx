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

    // ---- Reverb: an EON voice through the reverb work area changes output. ----
    int16_t dry[8192], wet[8192];
    for (int pass = 0; pass < 2; pass++) {
        int reverb_on = pass == 1;

        psx_spu_reset();
        wr16(MAINVOL_L, 0x7FFF);
        wr16(MAINVOL_R, 0x7FFF);

        // Upload the same looping full-scale ADPCM block.
        wr16(XFER_ADDR, 0x1000 / 8);
        wr16(XFER_FIFO, 0x0300);
        for (int i = 0; i < 7; i++) {
            wr16(XFER_FIFO, 0x1111);
        }
        wr16(0x0, 0x2000);             // voice volume L (leave headroom for reverb)
        wr16(0x2, 0x2000);             // voice volume R
        wr16(0x4, 0x1000);             // pitch
        wr16(0x6, 0x1000 / 8);        // start
        wr16(0x8, 0x000F);             // fast attack, sustain max
        wr16(0xA, 0x0000);
        wr32(0x198, 0x00000001);       // EON: voice 0 to reverb

        if (reverb_on) {
            // A real "Hall" reverb preset, in hardware register order (dAPF1,
            // dAPF2, vIIR, vCOMB1..4, vWALL, vAPF1, vAPF2, then the addresses).
            // set_reverb_reg maps each register offset to its field, so writing
            // the preset in order reproduces the hardware routing.
            static const uint16_t hall[32] = {
                0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
                0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
                0x0334, 0x01B5, 0x0BF0, 0x0C60, 0x0B08, 0x07D0, 0x0AB0, 0x0AE0,
                0x07A0, 0x0464, 0x0304, 0x0310, 0x0378, 0x038C, 0x8100, 0x8100,
            };
            wr16(0x1AA, 0x0080);       // SPUCNT reverb master enable
            wr16(0x1A2, 0xC000);       // mBASE -> StartAddr word 0x30000
            wr16(0x184, 0x4000);       // vLOUT
            wr16(0x186, 0x4000);       // vROUT
            for (int i = 0; i < 32; i++) {
                wr16(0x1C0 + i * 2, hall[i]);
            }
        }

        wr32(KON, 0x00000001);

        int got = 0;
        int16_t *dst = reverb_on ? wet : dry;
        while (got < 4000) {
            tick_samples(256);
            got += spu_output_pull(dst + got * 2, 256);
        }
    }

    int diff = 0;
    for (int i = 0; i < 4000 * 2; i++) {
        if (dry[i] != wet[i]) {
            diff++;
        }
    }
    if (diff == 0) {
        fprintf(stderr, "FAIL: reverb did not change the output\n");
        failures++;
    }

    // ---- Noise: a NON voice outputs the varying noise generator. ----
    psx_spu_reset();
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);
    wr16(SPUCNT, 0x3000);              // noise clock: shift 12, step 0
    wr32(0x194, 0x00000001);          // NON: voice 0 in noise mode
    wr16(0x0, 0x7FFF);                 // volume L
    wr16(0x2, 0x7FFF);                 // volume R
    wr16(0x8, 0x000F);                 // fast attack, sustain max
    wr16(0xA, 0x0000);
    wr32(KON, 0x00000001);

    tick_samples(4000);
    n = spu_output_pull(out, 4096);
    if (n > 4096) {
        n = 4096;
    }
    int noise_n = n;

    int distinct = 0, nz = 0;
    int16_t seen = out[0];
    for (int i = 0; i < n; i++) {
        if (out[i * 2] != 0) {
            nz++;
        }
        if (out[i * 2] != seen) {
            distinct++;
            seen = out[i * 2];
        }
    }
    if (!nz) {
        fprintf(stderr, "FAIL: noise voice produced silence\n");
        failures++;
    }
    if (distinct < 10) {
        fprintf(stderr, "FAIL: noise output does not vary (only %d changes)\n", distinct);
        failures++;
    }

    // ---- Pitch modulation: voice 0 modulates voice 1's pitch. ----
    int16_t pm_off[4096], pm_on[4096];
    for (int pass = 0; pass < 2; pass++) {
        int pmon = pass == 1;

        psx_spu_reset();
        wr16(MAINVOL_L, 0x7FFF);
        wr16(MAINVOL_R, 0x7FFF);

        // Voice 0 (the modulator): constant full-scale block, output muted so it
        // only modulates. Uploaded at sound-RAM 0x1000.
        wr16(XFER_ADDR, 0x1000 / 8);
        wr16(XFER_FIFO, 0x0300);
        for (int i = 0; i < 7; i++) {
            wr16(XFER_FIFO, 0x1111);
        }
        // Voice 1 (modulated): a square wave (nibbles alternate 1 and F).
        wr16(XFER_ADDR, 0x2000 / 8);
        wr16(XFER_FIFO, 0x0300);
        for (int i = 0; i < 7; i++) {
            wr16(XFER_FIFO, 0x1F1F);
        }

        wr16(0x0, 0x0000);            // voice 0 volume L (muted)
        wr16(0x2, 0x0000);            // voice 0 volume R
        wr16(0x4, 0x1000);            // voice 0 pitch
        wr16(0x6, 0x1000 / 8);       // voice 0 start
        wr16(0x8, 0x000F);
        wr16(0xA, 0x0000);

        wr16(0x10, 0x7FFF);           // voice 1 volume L
        wr16(0x12, 0x7FFF);           // voice 1 volume R
        wr16(0x14, 0x1000);           // voice 1 pitch (base)
        wr16(0x16, 0x2000 / 8);      // voice 1 start
        wr16(0x18, 0x000F);
        wr16(0x1A, 0x0000);

        wr32(0x190, pmon ? 0x00000002 : 0); // PMON: voice 1
        wr32(KON, 0x00000003);        // key on voices 0 and 1

        int got = 0;
        int16_t *dst = pmon ? pm_on : pm_off;
        while (got < 2000) {
            tick_samples(256);
            got += spu_output_pull(dst + got * 2, 256);
        }
    }

    int pm_diff = 0;
    for (int i = 0; i < 2000 * 2; i++) {
        if (pm_off[i] != pm_on[i]) {
            pm_diff++;
        }
    }
    if (pm_diff == 0) {
        fprintf(stderr, "FAIL: pitch modulation did not change voice 1\n");
        failures++;
    }

    // ---- Volume sweep: a sweep-increase on the left volume ramps amplitude. ----
    psx_spu_reset();
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);
    wr16(XFER_ADDR, 0x1000 / 8);
    wr16(XFER_FIFO, 0x0300);
    for (int i = 0; i < 7; i++) {
        wr16(XFER_FIFO, 0x1111);       // constant 4096 sample
    }
    wr16(0x0, 0x8020);                 // vol L: sweep mode, linear increase, rate 0x20
    wr16(0x2, 0x0000);                 // vol R: silent, isolate L
    wr16(0x4, 0x1000);
    wr16(0x6, 0x1000 / 8);
    wr16(0x8, 0x000F);
    wr16(0xA, 0x0000);
    wr32(KON, 0x00000001);

    tick_samples(2000);
    n = spu_output_pull(out, 4096);
    if (n > 2000) {
        n = 2000;
    }

    long early = 0, late = 0;
    for (int i = 0; i < 100; i++) {
        early += out[i * 2] < 0 ? -out[i * 2] : out[i * 2];
    }
    for (int i = n - 100; i < n; i++) {
        late += out[i * 2] < 0 ? -out[i * 2] : out[i * 2];
    }
    if (late <= early * 2) {
        fprintf(stderr, "FAIL: volume sweep did not ramp up (early=%ld late=%ld)\n",
                early, late);
        failures++;
    }

    // ---- CD audio to reverb: SPUCNT bit2 routes CD audio through reverb. ----
    // Feed the same CD signal twice, with and without the CD-reverb routing, and
    // confirm the routed pass differs (the reverb wet output is added).
    int16_t cddry[8192], cdwet[8192];
    int cd_diff = 0;
    for (int pass = 0; pass < 2; pass++) {
        int rev_on = pass == 1;

        psx_spu_reset();
        wr16(MAINVOL_L, 0x7FFF);
        wr16(MAINVOL_R, 0x7FFF);
        wr16(0x1B0, 0x7FFF);           // CD volume L
        wr16(0x1B2, 0x7FFF);           // CD volume R

        uint16_t spucnt = 0x0001;      // CD audio enable
        if (rev_on) {
            static const uint16_t hall[32] = {
                0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80,
                0x5800, 0x5300, 0x04D6, 0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF,
                0x0334, 0x01B5, 0x0BF0, 0x0C60, 0x0B08, 0x07D0, 0x0AB0, 0x0AE0,
                0x07A0, 0x0464, 0x0304, 0x0310, 0x0378, 0x038C, 0x8100, 0x8100,
            };
            spucnt |= 0x0004 | 0x0080; // CD-reverb routing + reverb master
            wr16(0x1A2, 0xC000);       // mBASE -> StartAddr word 0x30000
            wr16(0x184, 0x4000);       // vLOUT
            wr16(0x186, 0x4000);       // vROUT
            for (int i = 0; i < 32; i++) {
                wr16(0x1C0 + i * 2, hall[i]);
            }
        }
        wr16(SPUCNT, spucnt);

        int got = 0;
        int phase = 0;
        int16_t *dst = rev_on ? cdwet : cddry;
        while (got < 4000) {
            int16_t cdbuf[256 * 2];
            for (int i = 0; i < 256; i++) {
                int16_t s = ((phase++ >> 4) & 1) ? 0x4000 : -0x4000; // ~1.4 kHz square
                cdbuf[i * 2] = s;
                cdbuf[i * 2 + 1] = s;
            }
            spu_cd_audio(cdbuf, 256);
            tick_samples(256);
            got += spu_output_pull(dst + got * 2, 256);
        }
    }
    for (int i = 0; i < 4000 * 2; i++) {
        if (cddry[i] != cdwet[i]) {
            cd_diff++;
        }
    }
    if (cd_diff == 0) {
        fprintf(stderr, "FAIL: CD-to-reverb routing did not change the output\n");
        failures++;
    }

    if (failures) {
        fprintf(stderr, "SPU test: %d failures\n", failures);
        return 1;
    }
    printf("PASS: SPU CD mix and ADPCM voice synthesis correct (voice peak %d)\n",
           peak);
    printf("PASS: reverb alters output (%d/%d samples differ)\n", diff, 4000 * 2);
    printf("PASS: noise voice varies (%d changes over %d samples)\n", distinct, noise_n);
    printf("PASS: pitch modulation alters voice 1 (%d/%d samples differ)\n",
           pm_diff, 2000 * 2);
    printf("PASS: volume sweep ramps up (early=%ld late=%ld)\n", early, late);
    printf("PASS: CD audio routes through reverb (%d/%d samples differ)\n",
           cd_diff, 4000 * 2);
    return 0;
}
