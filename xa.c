#include "xa.h"
#include "spu.h"
#include <string.h>

// XA-ADPCM decoder. See xa.h for the interface.
//
// Sector layout: the 2304-byte sound-group area is 18 groups of 128 bytes. Each
// group is 16 header bytes followed by 112 data bytes. A group holds 8 ADPCM
// sound units of 28 samples each. Every data byte carries two 4-bit samples: the
// low nibble and the high nibble. The units interleave at byte granularity, so
// unit i (i = 0..3) reads sample n from data byte (i + 4 * n).
//
// In stereo the low nibble is a left sample and the high nibble the matching
// right sample, so the four units in a group give four left blocks and four right
// blocks. In mono the low nibble is one sample and the high nibble the next, so a
// unit yields 56 consecutive mono samples. Each unit's 4-bit filter/range byte is
// one of the header bytes, at header offsets {0,2,8,10} for the low-nibble block
// and {1,3,9,11} for the high-nibble block.
//
// The predictor is the same as the SPU ADPCM one, scaled by 1/64, but only the
// first four filters occur in XA data.

#define XA_GROUPS       18
#define XA_GROUP_BYTES  128
#define XA_UNIT_SAMPLES 28
#define XA_OUT_RATE     44100

static const int xa_filter_pos[4] = { 0, 60, 115, 98 };
static const int xa_filter_neg[4] = { 0, 0, -52, -55 };
static const int xa_head_off[4]   = { 0, 2, 8, 10 };

static struct {
    int32_t l_y0, l_y1;   // left/mono ADPCM predictor history
    int32_t r_y0, r_y1;   // right ADPCM predictor history

    // Linear resampler state. hist is the last output-space frame before the
    // sector being decoded, phase is a 16.16 fractional read position carried
    // between sectors, and have_hist marks the first sector so it starts clean.
    int16_t hist_l, hist_r;
    uint32_t phase;
    int have_hist;
} xa;

void
xa_reset(void)
{
    memset(&xa, 0, sizeof xa);
}

static int16_t
xa_clamp16(int32_t v)
{
    if (v < -32768) {
        return -32768;
    }
    if (v > 32767) {
        return 32767;
    }
    return (int16_t)v;
}

// Decode one 28-sample unit. `fr` is the filter/range byte, `d` the group's data
// area, `start`/`step` select the unit's data bytes, and `high` picks the high
// nibble. Samples are written to out with the given stride. y0/y1 carry the
// predictor history in and out.
static void
decode_unit(int32_t *y0, int32_t *y1, uint8_t fr, const uint8_t *d,
            int start, int high, int16_t *out, int stride)
{
    int filter = (fr >> 4) & 0x0f;
    int range  = fr & 0x0f;
    if (filter > 3) {
        filter = 3; // clamp so a malformed byte cannot index past the table
    }
    int fp = xa_filter_pos[filter];
    int fn = xa_filter_neg[filter];
    int32_t h0 = *y0, h1 = *y1;

    for (int n = 0; n < XA_UNIT_SAMPLES; n++) {
        uint8_t b = d[start + 4 * n];
        int nib = high ? (b >> 4) : (b & 0x0f);
        int32_t s = (int32_t)(int16_t)(nib << 12) >> range;
        int32_t v = s + ((h0 * fp + h1 * fn + 32) >> 6);
        v = xa_clamp16(v);
        h1 = h0;
        h0 = v;
        *out = (int16_t)v;
        out += stride;
    }

    *y0 = h0;
    *y1 = h1;
}

// Decode the sound groups to interleaved 16-bit stereo at the native rate.
// Returns the native frame count. Mono is expanded to stereo by duplication.
static int
decode_native(const uint8_t *groups, int stereo, int16_t *native)
{
    int frames = 0;

    for (int g = 0; g < XA_GROUPS; g++) {
        const uint8_t *H = groups + g * XA_GROUP_BYTES;
        const uint8_t *D = H + 16;

        for (int i = 0; i < 4; i++) {
            if (stereo) {
                int16_t l[XA_UNIT_SAMPLES], r[XA_UNIT_SAMPLES];
                decode_unit(&xa.l_y0, &xa.l_y1, H[xa_head_off[i] + 0], D, i, 0, l, 1);
                decode_unit(&xa.r_y0, &xa.r_y1, H[xa_head_off[i] + 1], D, i, 1, r, 1);
                for (int n = 0; n < XA_UNIT_SAMPLES; n++) {
                    native[frames * 2 + 0] = l[n];
                    native[frames * 2 + 1] = r[n];
                    frames++;
                }
            } else {
                int16_t a[XA_UNIT_SAMPLES], b[XA_UNIT_SAMPLES];
                decode_unit(&xa.l_y0, &xa.l_y1, H[xa_head_off[i] + 0], D, i, 0, a, 1);
                for (int n = 0; n < XA_UNIT_SAMPLES; n++) {
                    native[frames * 2 + 0] = a[n];
                    native[frames * 2 + 1] = a[n];
                    frames++;
                }
                decode_unit(&xa.l_y0, &xa.l_y1, H[xa_head_off[i] + 1], D, i, 1, b, 1);
                for (int n = 0; n < XA_UNIT_SAMPLES; n++) {
                    native[frames * 2 + 0] = b[n];
                    native[frames * 2 + 1] = b[n];
                    frames++;
                }
            }
        }
    }

    return frames;
}

void
xa_decode_sector(const uint8_t *groups, uint8_t coding)
{
    int stereo = (coding & 3) != 0;
    int freq   = ((coding >> 2) & 3) == 1 ? 18900 : 37800;
    int bps8   = ((coding >> 4) & 3) == 1;

    // A mono stream produces the most native frames (18 * 4 * 56 = 4032).
    int16_t native[XA_GROUPS * 4 * 2 * XA_UNIT_SAMPLES * 2];
    int frames;

    if (bps8) {
        // 8-bit XA is rare and not decoded yet. Emit silence of the right
        // length so the resampler and the stream timing stay consistent.
        frames = stereo ? XA_GROUPS * 4 * XA_UNIT_SAMPLES
                        : XA_GROUPS * 4 * 2 * XA_UNIT_SAMPLES;
        memset(native, 0, (size_t)frames * 2 * sizeof native[0]);
    } else {
        frames = decode_native(groups, stereo, native);
    }

    if (frames <= 0) {
        return;
    }

    // Resample native (freq) to XA_OUT_RATE and feed the SPU. The read position
    // advances by step per output frame; hist holds the frame just before
    // native[0] so interpolation is continuous across sector boundaries.
    uint32_t step = (uint32_t)(((uint64_t)freq << 16) / XA_OUT_RATE);

    if (!xa.have_hist) {
        xa.hist_l = native[0];
        xa.hist_r = native[1];
        xa.phase = 0;
        xa.have_hist = 1;
    }

    // Worst case: mono at 18900 gives 4032 native frames, 4032 * 44100 / 18900
    // = 9408 output frames.
    int16_t out[10240 * 2];
    int oframes = 0;
    uint32_t phase = xa.phase;

    while ((phase >> 16) < (uint32_t)frames && oframes < 10240) {
        int idx = (int)(phase >> 16);
        uint32_t frac = phase & 0xffff;

        int32_t l0 = (idx == 0) ? xa.hist_l : native[(idx - 1) * 2 + 0];
        int32_t r0 = (idx == 0) ? xa.hist_r : native[(idx - 1) * 2 + 1];
        int32_t l1 = native[idx * 2 + 0];
        int32_t r1 = native[idx * 2 + 1];

        out[oframes * 2 + 0] = (int16_t)(l0 + (((l1 - l0) * (int32_t)frac) >> 16));
        out[oframes * 2 + 1] = (int16_t)(r0 + (((r1 - r0) * (int32_t)frac) >> 16));
        oframes++;

        phase += step;
    }

    xa.phase = phase - ((uint32_t)frames << 16);
    xa.hist_l = native[(frames - 1) * 2 + 0];
    xa.hist_r = native[(frames - 1) * 2 + 1];

    if (oframes > 0) {
        spu_cd_audio(out, oframes);
    }
}
