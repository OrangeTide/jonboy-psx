// Host-side unit test for the SPU audio output path in spu.c: CD-audio volume
// scaling, the SPUCNT CD-enable gate, and the output ring buffer. spu.c has no
// other dependencies, so it links on its own.

#include "spu.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define SPUCNT_OFFSET  0x1AA
#define MAINVOL_L      0x180
#define MAINVOL_R      0x182
#define CDVOL_L        0x1B0
#define CDVOL_R        0x1B2

static void
wr16(uint32_t off, uint16_t v)
{
    psx_spu_write(SPU_BASE + off, v, 2);
}

// The same fixed-point scale spu.c applies, for computing expected output.
static int16_t
scale(int16_t sample, int vol, int main)
{
    int v = ((int)sample * vol) >> 15;
    v = (v * main) >> 15;
    return (int16_t)v;
}

static int failures;

static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d want %d\n", what, got, want);
        failures++;
    }
}

int
main(void)
{
    const int16_t in[8] = { 1000, -2000, 32767, -32768, 500, 600, -700, -800 };

    // 1. Full volume, CD enabled: output matches the scaled input.
    psx_spu_reset();
    wr16(SPUCNT_OFFSET, 0x0001);   // CD audio enable
    wr16(CDVOL_L, 0x7FFF);
    wr16(CDVOL_R, 0x7FFF);
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);
    spu_cd_audio(in, 4);

    int16_t out[16];
    int n = spu_output_pull(out, 16);
    check("full-volume frame count", n, 4);
    for (int i = 0; i < 4; i++) {
        int vol = (i % 2 == 0) ? 0x7FFF : 0x7FFF;
        check("full-volume sample", out[i * 2],
              scale(in[i * 2], vol, 0x7FFF));
        check("full-volume sample", out[i * 2 + 1],
              scale(in[i * 2 + 1], vol, 0x7FFF));
    }

    // 2. Half CD volume scales down.
    psx_spu_reset();
    wr16(SPUCNT_OFFSET, 0x0001);
    wr16(CDVOL_L, 0x4000);
    wr16(CDVOL_R, 0x4000);
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);
    spu_cd_audio(in, 2);
    n = spu_output_pull(out, 16);
    check("half-volume frame count", n, 2);
    check("half-volume L", out[0], scale(in[0], 0x4000, 0x7FFF));
    check("half-volume R", out[1], scale(in[1], 0x4000, 0x7FFF));

    // 3. CD audio disabled in SPUCNT: silence.
    psx_spu_reset();
    wr16(SPUCNT_OFFSET, 0x0000);    // CD audio disabled
    wr16(CDVOL_L, 0x7FFF);
    wr16(CDVOL_R, 0x7FFF);
    wr16(MAINVOL_L, 0x7FFF);
    wr16(MAINVOL_R, 0x7FFF);
    spu_cd_audio(in, 4);
    n = spu_output_pull(out, 16);
    check("disabled frame count", n, 4);
    for (int i = 0; i < 8; i++) {
        check("disabled is silent", out[i], 0);
    }

    // 4. Pull drains: a second pull with nothing queued returns 0.
    n = spu_output_pull(out, 16);
    check("drained pull returns 0", n, 0);

    if (failures) {
        fprintf(stderr, "SPU test: %d failures\n", failures);
        return 1;
    }
    printf("PASS: SPU CD-audio mixing, gate, and ring correct\n");
    return 0;
}
