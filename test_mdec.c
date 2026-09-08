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

    if (failures == 0) {
        printf("MDEC: all tests passed\n");
        return 0;
    }

    fprintf(stderr, "MDEC: %d failure(s)\n", failures);
    return 1;
}
