#include "xa.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Host unit test for the XA-ADPCM decoder. It builds synthetic sectors that use
// predictor filter 0 (no prediction) so every decoded sample is exactly the
// sign-extended data nibble shifted left by 12. That makes the decoded output
// exactly predictable, which lets the test check both the sample values and the
// resampled frame counts.

// Capture buffer for the SPU CD-audio input. xa.c calls spu_cd_audio; here it
// records the frames instead of mixing them.
#define CAP_MAX 65536
static int16_t cap[CAP_MAX * 2];
static int cap_frames;

void
spu_cd_audio(const int16_t *frames, int nframes)
{
    for (int i = 0; i < nframes && cap_frames < CAP_MAX; i++) {
        cap[cap_frames * 2 + 0] = frames[i * 2 + 0];
        cap[cap_frames * 2 + 1] = frames[i * 2 + 1];
        cap_frames++;
    }
}

// Build a 2304-byte sound-group area. Every header byte is 0 (filter 0, range 0)
// and every data byte is `fill`, so the low nibble drives one channel and the
// high nibble the other with a constant value.
static void
build_groups(uint8_t *groups, uint8_t fill)
{
    for (int g = 0; g < 18; g++) {
        uint8_t *G = groups + g * 128;
        memset(G, 0, 16);              // 16 header bytes: filter 0, range 0
        memset(G + 16, fill, 112);     // 112 data bytes
    }
}

static int failures;

static void
check(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) {
        failures++;
    }
}

int
main(void)
{
    uint8_t groups[2304];

    // Filter 0 with data nibble 1 (low) and 7 (high) gives 0x1000 = 4096 and
    // 0x7000 = 28672.
    const int16_t LOW = 0x1000;
    const int16_t HIGH = 0x7000;

    // ---- 4-bit stereo, 37800 Hz (coding 0x01) --------------------------------
    // low nibble -> left, high nibble -> right. 2016 native frames resample by
    // 44100/37800 = 7/6 to 2352 output frames, all (LOW, HIGH).
    xa_reset();
    cap_frames = 0;
    build_groups(groups, 0x71);
    xa_decode_sector(groups, 0x01);

    check(cap_frames > 2200 && cap_frames < 2500,
          "stereo 37800 frame count near 2352");
    check(cap[2 * 1000 + 0] == LOW && cap[2 * 1000 + 1] == HIGH,
          "stereo 37800 constant sample values");

    // ---- 4-bit stereo, 18900 Hz (coding 0x05) --------------------------------
    // Same 2016 native frames, resampled by 44100/18900 = 7/3 to 4704 frames.
    xa_reset();
    cap_frames = 0;
    xa_decode_sector(groups, 0x05);

    check(cap_frames > 4500 && cap_frames < 4900,
          "stereo 18900 frame count near 4704");
    check(cap[2 * 2000 + 0] == LOW && cap[2 * 2000 + 1] == HIGH,
          "stereo 18900 constant sample values");

    // ---- 4-bit mono, 37800 Hz (coding 0x00) ----------------------------------
    // A unit yields 28 low-nibble samples (LOW) then 28 high-nibble samples
    // (HIGH), duplicated to both channels. 4032 native frames resample to 4704.
    xa_reset();
    cap_frames = 0;
    xa_decode_sector(groups, 0x00);

    check(cap_frames > 4500 && cap_frames < 4900,
          "mono 37800 frame count near 4704");

    int mono_ok = 1, saw_low = 0, saw_high = 0;
    for (int i = 0; i < cap_frames; i++) {
        if (cap[i * 2] != cap[i * 2 + 1]) {
            mono_ok = 0;
        }
        if (cap[i * 2] == LOW) {
            saw_low = 1;
        }
        if (cap[i * 2] == HIGH) {
            saw_high = 1;
        }
    }
    check(mono_ok, "mono output has equal left and right");
    check(saw_low && saw_high, "mono output has both nibble levels");

    // ---- predictor history carries across sectors ----------------------------
    // Two identical constant sectors in a row must stay continuous, so a mid
    // sample of the second sector still reads the constant values.
    xa_reset();
    cap_frames = 0;
    xa_decode_sector(groups, 0x01);
    int first = cap_frames;
    xa_decode_sector(groups, 0x01);
    check(cap_frames > first,
          "second sector produced more output");
    check(cap[2 * (first + 500) + 0] == LOW && cap[2 * (first + 500) + 1] == HIGH,
          "stream stays continuous across sectors");

    // ---- 8-bit path is silent but keeps timing (coding 0x11) -----------------
    xa_reset();
    cap_frames = 0;
    xa_decode_sector(groups, 0x11);
    int all_zero = 1;
    for (int i = 0; i < cap_frames * 2; i++) {
        if (cap[i] != 0) {
            all_zero = 0;
        }
    }
    check(cap_frames > 2200 && cap_frames < 2500, "8-bit stereo frame count near 2352");
    check(all_zero, "8-bit path emits silence");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "OK",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
