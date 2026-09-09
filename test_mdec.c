// Host-side unit test for the MDEC decoder in mdec.c. It drives the two MMIO
// registers exactly as a game would: load the quant and scale tables, then
// feed run-length-coded blocks and read back the decoded pixels. mdec.c has no
// other dependencies, so it links on its own.
//
// The decode checks lean on a table-independent property of the separable
// IDCT: a block carrying only a DC coefficient reconstructs to a flat block
// (every pixel equal). That holds for the real PlayStation scale table, whose
// zeroth basis is constant, so the tests use that table and assert uniformity,
// the mid-gray point at zero, monotonicity in the DC term, and the output
// sizes for each colour depth.

#include "mdec.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// The standard PlayStation IDCT scale table (row 0 constant), fed to command 3
// as 64 signed 16-bit entries. Beetle and the retail libraries use this table.
static const uint16_t std_scale[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6E, 0x471D, 0x18F9, 0xE707, 0xB8E3, 0x9592, 0x8276,
    0x7642, 0x30FC, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FC, 0x7642,
    0x6A6E, 0xE707, 0x8276, 0xB8E3, 0x471D, 0x7D8A, 0x18F9, 0x9592,
    0x5A82, 0xA57E, 0xA57E, 0x5A82, 0x5A82, 0xA57E, 0xA57E, 0x5A82,
    0x471D, 0x8276, 0x18F9, 0x6A6E, 0x9592, 0xE707, 0x7D8A, 0xB8E3,
    0x30FC, 0x89BE, 0x7642, 0xCF04, 0xCF04, 0x7642, 0x89BE, 0x30FC,
    0x18F9, 0xB8E3, 0x6A6E, 0x9592, 0x7D8A, 0x471D, 0x8276, 0xE707,
};

static int failures;

static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d (0x%X) want %d (0x%X)\n",
                what, got, got, want, want);
        failures++;
    }
}

static void
wr0(uint32_t v)
{
    mdec_write(MDEC_BASE + 0, v);
}
static void
wr1(uint32_t v)
{
    mdec_write(MDEC_BASE + 4, v);
}
static uint32_t
rd0(void)
{
    return mdec_read(MDEC_BASE + 0);
}
static uint32_t
rd1(void)
{
    return mdec_read(MDEC_BASE + 4);
}

// Load the quant tables (all entries q) with command 2, colour table = luma.
static void
load_quant(uint8_t q)
{
    uint8_t table[64];
    int i;

    memset(table, q, sizeof(table));
    wr0(2u << 29); // command 2, luma only

    for (i = 0; i < 64; i += 4) {
        uint32_t word = table[i] | (table[i + 1] << 8)
                        | (table[i + 2] << 16) | ((uint32_t)table[i + 3] << 24);
        wr0(word);
    }
}

// Load the standard scale table with command 3.
static void
load_scale(void)
{
    int i;

    wr0(3u << 29); // command 3

    for (i = 0; i < 64; i += 2)
        wr0(std_scale[i] | ((uint32_t)std_scale[i + 1] << 16));
}

// Build a decode command word: op 1, given depth/sign/bit15 and word count.
static uint32_t
decode_cmd(uint32_t depth, uint32_t sign, uint32_t bit15, uint32_t words)
{
    return (1u << 29) | (depth << 27) | (sign << 26) | (bit15 << 25)
           | (words & 0xFFFFu);
}

// Decode one solid-colour 16x16 macroblock: six DC-only blocks in the hardware
// order (Cr, Cb, Y0..Y3), each an unquantized DC (q_scale 0) so it reconstructs
// to a flat block. 24-bit output, so the result is 16*16*3 = 768 bytes. Returns
// the number of output bytes read.
static int
decode_color_mb(int dc_cr, int dc_cb, int dc_y, uint8_t out[768])
{
    int dcs[6] = { dc_cr, dc_cb, dc_y, dc_y, dc_y, dc_y };
    int i, n = 0;

    wr0(decode_cmd(2, 0, 0, 6)); // depth 2 = 24-bit, unsigned

    for (i = 0; i < 6; i++) {
        uint16_t hw0 = (uint16_t)(dcs[i] & 0x3FF); // q_scale 0, DC value
        wr0(hw0 | ((uint32_t)0xFE00u << 16));      // then end-of-block
    }

    while (mdec_dma_out_ready() && n < 768) {
        uint32_t w = mdec_dma_out();
        out[n++] = (uint8_t)w;

        if (n < 768) out[n++] = (uint8_t)(w >> 8);

        if (n < 768) out[n++] = (uint8_t)(w >> 16);

        if (n < 768) out[n++] = (uint8_t)(w >> 24);
    }

    return n;
}

// Feed one monochrome (8-bit) DC-only block and read back the 64 output bytes.
// The block is one word: DC coefficient with q_scale in the high bits, then an
// end-of-block marker. Two 16-bit halfwords pack into one 32-bit word.
static void
decode_dc_block_8bit(int q_scale, int dc, uint8_t out[64])
{
    uint16_t hw0 = (uint16_t)(((q_scale & 0x3F) << 10) | (dc & 0x3FF));
    uint16_t hw1 = 0xFE00; // end of block
    int i;

    // depth 1 = 8-bit, unsigned. One command word of parameters (two halfwords).
    wr0(decode_cmd(1, 0, 0, 1));
    wr0(hw0 | ((uint32_t)hw1 << 16));

    for (i = 0; i < 64; i += 4) {
        uint32_t w = rd0();
        out[i + 0] = (uint8_t)w;
        out[i + 1] = (uint8_t)(w >> 8);
        out[i + 2] = (uint8_t)(w >> 16);
        out[i + 3] = (uint8_t)(w >> 24);
    }
}

// ---------------------------------------------------------------------------
// Byte-exact colour-path pin against an independent Beetle reference.
//
// The property checks above use DC-only blocks, so they never exercise the AC
// path: the zig-zag scatter, the AC dequant, or a non-constant IDCT basis. This
// pin feeds an aggressive six-block colour macroblock (many AC terms, saturating
// levels, high zig-zag indices, and end-of-block markers) and diffs the packed
// output byte for byte against a reference pipeline that recomputes the frame a
// second, independent way: the dequant (fixed-point domain, sign bias, clamp),
// the YCbCr-to-RGB conversion (green-channel masking and rounding), and the 24-
// and 15-bit packing (the uint8 truncation before the RGB555 round) are all
// transcribed from the mednafen/Beetle MDEC, not from mdec.c, so a drift in any
// of them is caught. The two-pass IDCT mirrors the decoder's own transpose
// convention, whose orientation the amidog DCTBlockDecode ROMs already validate
// against real hardware. All six blocks use a nonzero q_scale (the quantized
// path a real FMV uses); the raw q_scale == 0 path is covered by amidog.

static const uint8_t ref_zig[64] = {
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11,
    0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14,
    0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24,
    0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35,
    0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f,
};

static int16_t ref_mat[64];

// Sign-extend the low bits bits of v.
static int
ref_sx(int bits, int v)
{
    int s = 32 - bits;
    return (int)(((int32_t)((uint32_t)v << s)) >> s);
}

static int
ref_s10(int v)
{
    return (int16_t)ref_sx(10, v);
}

// 9-bit mask then clamp to signed 8-bit, the MDEC's intermediate ringing rule.
static int
ref_mask9(int v)
{
    v = ref_sx(9, v);

    if (v < -128) return -128;

    if (v > 127) return 127;

    return v;
}

static int
ref_clamp(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// The SET_SCALE loader stores each entry pre->>3 at a transposed index.
static void
ref_set_scale(void)
{
    int i;

    for (i = 0; i < 64; i++)
        ref_mat[((i & 7) << 3) | ((i >> 3) & 7)] =
            (int16_t)((int16_t)std_scale[i] >> 3);
}

// Run-length expand and dequantize one block (quantized path only).
static void
ref_dequant(int16_t *coeff, const uint16_t *rle, int n, const uint8_t *quant)
{
    int p = 0, ci = 0, qscale = 0;

    memset(coeff, 0, 64 * sizeof(int16_t));

    while (p < n) {
        uint16_t word = rle[p++];

        if (ci == 0) {
            if (word == 0xFE00u) continue;

            qscale = word >> 10;
            int c = ref_s10(word & 0x3FFu);
            int q = quant[0];
            int t = q ? (((c * q) << 4) + (c ? (c < 0 ? 8 : -8) : 0))
                      : ((c * 2) << 4);
            coeff[ref_zig[0]] = (int16_t)ref_clamp(t, -0x4000, 0x3FFF);
            ci++;
        } else if (word == 0xFE00u) {
            while (ci < 64) coeff[ref_zig[ci++]] = 0;
        } else {
            uint32_t run = word >> 10;

            for (uint32_t k = 0; k < run && ci < 64; k++)
                coeff[ref_zig[ci++]] = 0;

            if (ci < 64) {
                int c = ref_s10(word & 0x3FFu);
                int q = qscale * quant[ci];
                int t = q ? ((((c * q) >> 3) << 4) + (c ? (c < 0 ? 8 : -8) : 0))
                          : ((c * 2) << 4);
                coeff[ref_zig[ci]] = (int16_t)ref_clamp(t, -0x4000, 0x3FFF);
                ci++;
            }
        }
    }
}

// Two separable IDCT passes: pass one keeps int16 and transposes, pass two masks
// to signed 8-bit. Rounding is (sum + 0x4000) >> 15.
static void
ref_idct(const int16_t *in, int16_t *out)
{
    int16_t tmp[64];
    int col, x, u;

    for (col = 0; col < 8; col++)
        for (x = 0; x < 8; x++) {
            int s = 0;

            for (u = 0; u < 8; u++)
                s += (int)in[col * 8 + u] * (int)ref_mat[x * 8 + u];

            tmp[x * 8 + col] = (int16_t)((s + 0x4000) >> 15);
        }

    for (col = 0; col < 8; col++)
        for (x = 0; x < 8; x++) {
            int s = 0;

            for (u = 0; u < 8; u++)
                s += (int)tmp[col * 8 + u] * (int)ref_mat[x * 8 + u];

            out[x * 8 + col] = (int16_t)ref_mask9((s + 0x4000) >> 15);
        }
}

// YCbCr to unsigned RGB with the /256 coefficients, +0x80 rounding, and the
// reduced-precision green channel; the ^0x80 returns each channel to 0..255.
static void
ref_yuv(int8_t y, int8_t cb, int8_t cr, int *r, int *g, int *b)
{
    *r = ref_mask9(y + (((359 * cr) + 0x80) >> 8));
    *g = ref_mask9(y + ((((-88 * cb) & ~0x1F)
                         + ((-183 * cr) & ~0x07) + 0x80) >> 8));
    *b = ref_mask9(y + (((454 * cb) + 0x80) >> 8));
    *r ^= 0x80;
    *g ^= 0x80;
    *b ^= 0x80;
}

// 8-bit unsigned channel to 5-bit. The uint8_t parameter truncates the ^0x80
// result before the round and shift, which is the detail that makes this a true
// oracle (an int parameter would reproduce a decoder bug rather than catch it).
static int
ref_555(uint8_t c)
{
    int v = (c + 4) >> 3;
    return v > 0x1F ? 0x1F : v;
}

// The aggressive input, shared by the reference and the real decoder.
static uint16_t pin_blk[6][16];
static int pin_blkn[6];
static uint8_t pin_qy[64], pin_qc[64];

static void
build_pin_input(void)
{
    int b, i;

    for (i = 0; i < 64; i++) {
        pin_qy[i] = (uint8_t)(2 + (i % 17));
        pin_qc[i] = (uint8_t)(3 + (i % 13));
    }

    // Six distinct blocks with a run of AC terms: a large DC, saturating and
    // negative levels, and runs that reach high zig-zag indices, ended by EOB.
    for (b = 0; b < 6; b++) {
        int n = 0, qs = 8 + b;

        pin_blk[b][n++] = (uint16_t)((qs << 10) | ((400 - 37 * b) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((0u << 10) | ((uint16_t)(-300 + 11 * b) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((1u << 10) | ((250 - 9 * b) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((0u << 10) | ((uint16_t)(-120) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((4u << 10) | ((333 + b) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((2u << 10) | ((uint16_t)(-77) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((7u << 10) | (90 & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((11u << 10) | ((uint16_t)(-200) & 0x3FF));
        pin_blk[b][n++] = (uint16_t)((1u << 10) | (45 & 0x3FF));
        pin_blk[b][n++] = 0xFE00u;
        pin_blkn[b] = n;
    }
}

// Reference macroblock: decode the six blocks and assemble the 16x16 pixels the
// same way the hardware does (Y quadrants, 2x2 chroma subsampling), packing to
// 24-bit (three bytes) and 15-bit (RGB555, little-endian). Unsigned output.
static void
build_reference(uint8_t out24[768], uint8_t out15[512])
{
    int16_t crc[64], cbc[64], yc[4][64];
    int16_t cr8[64], cb8[64], y8[4][64];
    int px, py, k, o24 = 0, o15 = 0;

    ref_set_scale();
    ref_dequant(crc, pin_blk[0], pin_blkn[0], pin_qc);
    ref_idct(crc, cr8);
    ref_dequant(cbc, pin_blk[1], pin_blkn[1], pin_qc);
    ref_idct(cbc, cb8);

    for (k = 0; k < 4; k++) {
        ref_dequant(yc[k], pin_blk[2 + k], pin_blkn[2 + k], pin_qy);
        ref_idct(yc[k], y8[k]);
    }

    for (py = 0; py < 16; py++)
        for (px = 0; px < 16; px++) {
            int yb = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int lx = px & 7, ly = py & 7;
            int chroma = (px >> 1) + (py >> 1) * 8;
            int r, g, b;

            ref_yuv((int8_t)y8[yb][lx + ly * 8], (int8_t)cb8[chroma],
                    (int8_t)cr8[chroma], &r, &g, &b);

            out24[o24++] = (uint8_t)r;
            out24[o24++] = (uint8_t)g;
            out24[o24++] = (uint8_t)b;

            uint16_t packed = (uint16_t)(ref_555((uint8_t)r)
                                         | (ref_555((uint8_t)g) << 5)
                                         | (ref_555((uint8_t)b) << 10));
            out15[o15++] = (uint8_t)packed;
            out15[o15++] = (uint8_t)(packed >> 8);
        }
}

// Load the luma and chroma quant tables with command 2 (both tables).
static void
load_quant_tables(const uint8_t *qy, const uint8_t *qc)
{
    uint8_t bytes[128];
    int i;

    memcpy(bytes, qy, 64);
    memcpy(bytes + 64, qc, 64);
    wr0((2u << 29) | 1u); // command 2, both tables

    for (i = 0; i < 128; i += 4)
        wr0(bytes[i] | (bytes[i + 1] << 8) | (bytes[i + 2] << 16)
            | ((uint32_t)bytes[i + 3] << 24));
}

// Drive the real decoder over the pin blocks at the given depth and read the
// packed output back through the DMA port. Returns the number of bytes read.
static int
run_pin_decoder(uint32_t depth, uint8_t *out, int max)
{
    uint16_t hw[128];
    int hn = 0, b, i, n = 0;

    for (b = 0; b < 6; b++)
        for (i = 0; i < pin_blkn[b]; i++)
            hw[hn++] = pin_blk[b][i];

    if (hn & 1) hw[hn++] = 0xFE00u; // pad to an even halfword count

    wr0(decode_cmd(depth, 0, 0, (uint32_t)(hn / 2)));

    for (i = 0; i < hn; i += 2)
        wr0(hw[i] | ((uint32_t)hw[i + 1] << 16));

    while (mdec_dma_out_ready() && n < max) {
        uint32_t w = mdec_dma_out();

        out[n++] = (uint8_t)w;

        if (n < max) out[n++] = (uint8_t)(w >> 8);

        if (n < max) out[n++] = (uint8_t)(w >> 16);

        if (n < max) out[n++] = (uint8_t)(w >> 24);
    }

    return n;
}

static void
test_colour_pin(void)
{
    static uint8_t ref24[768], ref15[512];
    static uint8_t got24[768], got15[512];
    int n24, n15, d24 = 0, d15 = 0, i;

    build_pin_input();
    build_reference(ref24, ref15);

    mdec_reset();
    load_scale();
    load_quant_tables(pin_qy, pin_qc);

    n24 = run_pin_decoder(2, got24, sizeof got24);
    n15 = run_pin_decoder(3, got15, sizeof got15);

    check("pin: 24-bit output is 768 bytes", n24, 768);
    check("pin: 15-bit output is 512 bytes", n15, 512);

    for (i = 0; i < 768; i++)
        if (got24[i] != ref24[i]) d24++;

    for (i = 0; i < 512; i++)
        if (got15[i] != ref15[i]) d15++;

    // On a mismatch, show the first differing byte to localize the stage.
    if (d24)
        for (i = 0; i < 768; i++)
            if (got24[i] != ref24[i]) {
                fprintf(stderr, "  pin 24-bit first diff at byte %d: "
                        "got 0x%02X want 0x%02X\n", i, got24[i], ref24[i]);
                break;
            }

    if (d15)
        for (i = 0; i < 512; i++)
            if (got15[i] != ref15[i]) {
                fprintf(stderr, "  pin 15-bit first diff at byte %d: "
                        "got 0x%02X want 0x%02X\n", i, got15[i], ref15[i]);
                break;
            }

    check("pin: 24-bit macroblock matches Beetle reference", d24, 0);
    check("pin: 15-bit macroblock matches Beetle reference", d15, 0);
}

int
main(void)
{
    uint8_t block[64];
    int i;

    // ---- Reset state and status register ----
    mdec_reset();
    // Fresh MDEC: output empty (bit 31), no output request, depth 3 (15-bit).
    uint32_t st = rd1();
    check("reset: output empty bit31", (st >> 31) & 1, 1);
    check("reset: depth is 3", (st >> 25) & 3, 3);
    check("reset: not busy", (st >> 29) & 1, 0);
    check("reset: no data-out ready", mdec_dma_out_ready(), 0);

    // ---- Control writes set the DMA-request enables ----
    wr1(0x40000000); // enable data-in (DMA0)
    // With input FIFO ready and DMA-in enabled, bit 28 is set once a command is
    // collecting. Idle FIFO reports full (bit 30) since there is nothing to take.
    load_scale();
    load_quant(8);

    // ---- DC-only block reconstructs to a flat block ----
    decode_dc_block_8bit(1, 200, block);
    int uniform = 1;

    for (i = 1; i < 64; i++)
        if (block[i] != block[0]) uniform = 0;

    check("dc-only block is uniform", uniform, 1);

    // Zero DC (unsigned output) is mid-gray, 0x80.
    decode_dc_block_8bit(1, 0, block);
    check("zero DC is mid-gray", block[0], 0x80);

    // A larger positive DC yields a larger (brighter) uniform value; a negative
    // DC yields a darker one. Monotonic around the mid-gray point.
    uint8_t lo[64], mid[64], hi[64];
    decode_dc_block_8bit(1, -200, lo);
    decode_dc_block_8bit(1, 0, mid);
    decode_dc_block_8bit(1, 200, hi);
    check("negative DC below mid-gray", lo[0] < mid[0], 1);
    check("positive DC above mid-gray", hi[0] > mid[0], 1);

    // ---- Signed 8-bit output shifts the zero point to 0 ----
    wr0(decode_cmd(1, 1, 0, 1)); // depth 1, signed
    wr0(0x0400u | ((uint32_t)0xFE00u << 16)); // q_scale 1, DC 0, then EOB
    {
        uint32_t w = rd0();
        check("signed zero DC is 0", (int8_t)(uint8_t)w, 0);
    }

    // ---- Output sizes per depth for a colour macroblock ----
    // A colour macroblock is six blocks (Cr, Cb, Y0..Y3). Feed six DC-only
    // blocks; 24-bit output is 16*16*3 = 768 bytes, 15-bit is 512 bytes.
    for (int depth = 2; depth <= 3; depth++) {
        // Six blocks, each two halfwords, so six words of parameters.
        wr0(decode_cmd((uint32_t)depth, 0, 0, 6));

        for (i = 0; i < 6; i++)
            wr0(0x0400u | ((uint32_t)0xFE00u << 16)); // DC 0 + EOB

        int bytes = 0;

        while (mdec_dma_out_ready()) {
            (void)mdec_dma_out();
            bytes += 4;
        }

        check(depth == 3 ? "15-bit macroblock is 512 bytes"
              : "24-bit macroblock is 768 bytes",
              bytes, depth == 3 ? 512 : 768);
    }

    // ---- Colour macroblock: solid-colour reconstruction and hue ----
    // DC-only blocks give a flat 16x16 macroblock, so every one of the 256
    // pixels must be the same RGB triple: this exercises the six-block assembly,
    // chroma subsampling, and YCbCr-to-RGB. Neutral chroma is grey; positive Cr
    // pushes red above blue, positive Cb pushes blue above red.
    mdec_reset();
    load_scale();
    {
        uint8_t mb[768];
        int n = decode_color_mb(0, 0, 300, mb); // neutral chroma, bright luma
        check("colour macroblock is 768 bytes", n, 768);

        int uniform = 1;

        for (i = 3; i < 768; i++)
            if (mb[i] != mb[i % 3]) uniform = 0;

        check("solid colour macroblock is uniform", uniform, 1);

        // Neutral chroma reconstructs to grey: R, G, and B all equal.
        check("neutral chroma is grey (R==B)", mb[0], mb[2]);
        check("neutral chroma is grey (R==G)", mb[0], mb[1]);

        // Positive Cr is reddish (R > B); positive Cb is bluish (B > R).
        decode_color_mb(240, 0, 300, mb);
        check("positive Cr is reddish", mb[0] > mb[2], 1);
        decode_color_mb(0, 240, 300, mb);
        check("positive Cb is bluish", mb[2] > mb[0], 1);
    }

    // ---- Status reports remaining parameter words while collecting ----
    mdec_reset();
    load_scale();
    wr0(decode_cmd(2, 0, 0, 4)); // a 4-word decode, none fed yet
    st = rd1();
    check("busy while collecting", (st >> 29) & 1, 1);
    // 4 words expected, 0 collected: remaining-1 in the low 16 bits.
    check("remaining word count", st & 0xFFFF, 3);
    wr0(0); // feed 2 halfwords (1 word)
    st = rd1();
    check("remaining after one word", st & 0xFFFF, 2);

    // ---- Byte-exact colour AC path vs an independent Beetle reference ----
    test_colour_pin();

    if (failures == 0) {
        printf("MDEC: all tests passed\n");
        return 0;
    }

    fprintf(stderr, "MDEC: %d failure(s)\n", failures);
    return 1;
}
