// Oracle cross-check: jonboy's MDEC (mdec.c) against the real mednafen/Beetle
// MDEC (REF/beetle-mdec/psx/mdec.c), byte for byte, on an identical aggressive
// colour macroblock. jonboy's decoder is a transcription of the Beetle pipeline,
// so this is the check the self-transcribed pin in test_mdec.c cannot make: it
// compiles Beetle's actual code as a second decoder and diffs the two, catching
// any bug they would otherwise share.
//
// REF-gated: built only by `make mdec-oracle`, which needs the Beetle source
// fetched under REF/beetle-mdec (git-ignored, like the conformance ROMs).

#include "mdec.h" // jonboy's decoder API

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Beetle's public MDEC API (REF/beetle-mdec/psx/mdec.h), linked in alongside.
void MDEC_Power(void);
void MDEC_Write(const int32_t timestamp, uint32_t A, uint32_t V);
uint32_t MDEC_Read(const int32_t timestamp, uint32_t A);
uint32_t MDEC_DMARead(uint32_t *offs);
void MDEC_Run(int32_t clocks);

// Beetle references this extern (normally the PSX event scheduler's budget). A
// large value keeps MDEC_Run from clock-starving the decode. Defined here so the
// Beetle object links without the rest of the mednafen core.
int32_t EventCycles = 0x40000000;

// Beetle's MDEC_StateAction references the save-state machinery, which the
// oracle never invokes. Stub the one symbol so the object links standalone.
int
MDFNSS_StateAction(void *st, int load, int data_only, void *sf, const char *name)
{
    (void)st, (void)load, (void)data_only, (void)sf, (void)name;
    return 1;
}

static int failures;

// The standard PlayStation IDCT scale table (same one test_mdec.c uses).
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

// The aggressive six-block macroblock, matching test_mdec.c's pin input.
static uint16_t blk[6][16];
static int blkn[6];
static uint8_t qy[64], qc[64];

static void
build_input(void)
{
    int b, i;

    for (i = 0; i < 64; i++) {
        qy[i] = (uint8_t)(2 + (i % 17));
        qc[i] = (uint8_t)(3 + (i % 13));
    }

    for (b = 0; b < 6; b++) {
        int n = 0, qs = 8 + b;

        blk[b][n++] = (uint16_t)((qs << 10) | ((400 - 37 * b) & 0x3FF));
        blk[b][n++] = (uint16_t)((0u << 10) | ((uint16_t)(-300 + 11 * b) & 0x3FF));
        blk[b][n++] = (uint16_t)((1u << 10) | ((250 - 9 * b) & 0x3FF));
        blk[b][n++] = (uint16_t)((0u << 10) | ((uint16_t)(-120) & 0x3FF));
        blk[b][n++] = (uint16_t)((4u << 10) | ((333 + b) & 0x3FF));
        blk[b][n++] = (uint16_t)((2u << 10) | ((uint16_t)(-77) & 0x3FF));
        blk[b][n++] = (uint16_t)((7u << 10) | (90 & 0x3FF));
        blk[b][n++] = (uint16_t)((11u << 10) | ((uint16_t)(-200) & 0x3FF));
        blk[b][n++] = (uint16_t)((1u << 10) | (45 & 0x3FF));
        blk[b][n++] = 0xFE00u;
        blkn[b] = n;
    }
}

// Pack the command word plus its parameter halfwords into 32-bit words.
static int
pack_words(uint32_t cmd, const uint16_t *hw, int nhw, uint32_t *out)
{
    int n = 0, i;

    out[n++] = cmd;

    for (i = 0; i < nhw; i += 2) {
        uint32_t lo = hw[i];
        uint32_t hi = (i + 1 < nhw) ? hw[i + 1] : 0xFE00u;
        out[n++] = lo | (hi << 16);
    }

    return n;
}

// Assemble the six blocks' halfwords into one flat stream.
static int
decode_stream(uint16_t *hw)
{
    int b, i, n = 0;

    for (b = 0; b < 6; b++)
        for (i = 0; i < blkn[b]; i++)
            hw[n++] = blk[b][i];

    return n;
}

// ---------------------------------------------------------------------------
// jonboy decoder
// ---------------------------------------------------------------------------
static int
jonboy_decode(uint32_t depth, uint8_t *out, int max)
{
    uint8_t qbytes[128];
    uint16_t hw[128];
    int i, nhw, n = 0;

    mdec_reset();

    // SET_QUANT (both tables).
    memcpy(qbytes, qy, 64);
    memcpy(qbytes + 64, qc, 64);
    mdec_write(MDEC_BASE, (2u << 29) | 1u);

    for (i = 0; i < 128; i += 4)
        mdec_write(MDEC_BASE, qbytes[i] | (qbytes[i + 1] << 8)
                   | (qbytes[i + 2] << 16) | ((uint32_t)qbytes[i + 3] << 24));

    // SET_SCALE.
    mdec_write(MDEC_BASE, 3u << 29);

    for (i = 0; i < 64; i += 2)
        mdec_write(MDEC_BASE, std_scale[i] | ((uint32_t)std_scale[i + 1] << 16));

    // DECODE.
    nhw = decode_stream(hw);

    if (nhw & 1) hw[nhw++] = 0xFE00u;

    mdec_write(MDEC_BASE, (1u << 29) | (depth << 27) | (uint32_t)(nhw / 2));

    for (i = 0; i < nhw; i += 2)
        mdec_write(MDEC_BASE, hw[i] | ((uint32_t)hw[i + 1] << 16));

    while (mdec_dma_out_ready() && n < max) {
        uint32_t w = mdec_dma_out();

        out[n++] = (uint8_t)w;

        if (n < max) out[n++] = (uint8_t)(w >> 8);

        if (n < max) out[n++] = (uint8_t)(w >> 16);

        if (n < max) out[n++] = (uint8_t)(w >> 24);
    }

    return n;
}

// ---------------------------------------------------------------------------
// Beetle decoder driver (through its real public API)
// ---------------------------------------------------------------------------
#define BE_STATUS() MDEC_Read(0, 4)
#define BE_OUT_EMPTY (1u << 31)
#define BE_IN_FULL   (1u << 30)
#define BE_IN_CMD    (1u << 29)

// Feed one command (command word + params) and collect any output. A single
// pump interleaves feeding, running, and draining so neither FIFO deadlocks.
static int
beetle_command(const uint32_t *words, int nwords, uint8_t *out, int max)
{
    int wi = 0, n = 0;
    long guard = 0;

    for (;;) {
        uint32_t offs, st = BE_STATUS();

        // Drain output first so the decode never stalls on a full out-FIFO.
        while (!(BE_STATUS() & BE_OUT_EMPTY)) {
            uint32_t v = MDEC_DMARead(&offs);

            if (n < max) out[n++] = (uint8_t)v;

            if (n < max) out[n++] = (uint8_t)(v >> 8);

            if (n < max) out[n++] = (uint8_t)(v >> 16);

            if (n < max) out[n++] = (uint8_t)(v >> 24);
        }

        st = BE_STATUS();

        if (wi < nwords && !(st & BE_IN_FULL))
            MDEC_Write(0, 0, words[wi++]);

        MDEC_Run(0x1000000);

        st = BE_STATUS();

        if (wi >= nwords && (st & BE_OUT_EMPTY) && !(st & BE_IN_CMD))
            break;

        if (guard++ > 2000000) {
            fprintf(stderr, "beetle driver: guard tripped\n");
            break;
        }
    }

    return n;
}

static int
beetle_decode(uint32_t depth, uint8_t *out, int max)
{
    uint8_t qbytes[128];
    uint16_t hw[128];
    uint32_t words[80];
    int i, nhw, nw;

    MDEC_Write(0, 4, 0x80000000u); // reset

    // SET_QUANT (both tables): pack 128 bytes into 32 words.
    memcpy(qbytes, qy, 64);
    memcpy(qbytes + 64, qc, 64);
    {
        uint16_t qhw[64];

        for (i = 0; i < 64; i++)
            qhw[i] = qbytes[i * 2] | (qbytes[i * 2 + 1] << 8);

        nw = pack_words((2u << 29) | 1u, qhw, 64, words);
        beetle_command(words, nw, NULL, 0);
    }

    // SET_SCALE.
    nw = pack_words(3u << 29, std_scale, 64, words);
    beetle_command(words, nw, NULL, 0);

    // DECODE.
    nhw = decode_stream(hw);
    nw = pack_words((1u << 29) | (depth << 27) | (uint32_t)((nhw + (nhw & 1)) / 2),
                    hw, nhw, words);
    return beetle_command(words, nw, out, max);
}

// Beetle outputs a colour macroblock as four sequential 8x8 quadrant blocks
// (Y0, Y1, Y2, Y3); the hardware DMA's per-word offset scatters them into the
// 16x16 raster the frame buffer holds. jonboy bakes that raster order into its
// emit and uses a plain linear DMA, so to compare the decoded pixels we first
// reorder Beetle's quadrant stream into the same raster.
static void
beetle_to_raster(const uint8_t *in, uint8_t *out, int bpp)
{
    int py, px, c;

    for (py = 0; py < 16; py++)
        for (px = 0; px < 16; px++) {
            int q = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int src = (q * 64 + (py & 7) * 8 + (px & 7)) * bpp;
            int dst = (py * 16 + px) * bpp;

            for (c = 0; c < bpp; c++)
                out[dst + c] = in[src + c];
        }
}

static void
compare(uint32_t depth, const char *name, int expect)
{
    static uint8_t jout[768], bout[768], braster[768];
    int nj, nb, i, diffs = 0, firstdiff = -1;

    nj = jonboy_decode(depth, jout, sizeof jout);
    nb = beetle_decode(depth, bout, sizeof bout);
    beetle_to_raster(bout, braster, depth == 2 ? 3 : 2);
    memcpy(bout, braster, sizeof bout);

    if (nj != expect) {
        fprintf(stderr, "FAIL: %s jonboy length %d, want %d\n", name, nj, expect);
        failures++;
    }

    if (nb != expect) {
        fprintf(stderr, "FAIL: %s Beetle length %d, want %d\n", name, nb, expect);
        failures++;
    }

    int m = nj < nb ? nj : nb;

    for (i = 0; i < m; i++)
        if (jout[i] != bout[i]) {
            if (firstdiff < 0) firstdiff = i;

            diffs++;
        }

    if (diffs) {
        fprintf(stderr, "FAIL: %s: %d/%d bytes differ; first at %d: "
                "jonboy 0x%02X Beetle 0x%02X\n", name, diffs, m, firstdiff,
                jout[firstdiff], bout[firstdiff]);

        // Diagnostic: is jonboy's output the spatial transpose of Beetle's?
        int bpp = (depth == 2) ? 3 : 2, tdiff = 0, px, py, c;

        for (py = 0; py < 16; py++)
            for (px = 0; px < 16; px++)
                for (c = 0; c < bpp; c++) {
                    int ji = (py * 16 + px) * bpp + c;
                    int bi = (px * 16 + py) * bpp + c;

                    if (ji < m && bi < m && jout[ji] != bout[bi]) tdiff++;
                }

        fprintf(stderr, "       transpose(jonboy) vs Beetle: %d bytes differ\n",
                tdiff);
        failures++;
    } else {
        printf("  %s: %d bytes byte-exact vs Beetle\n", name, m);
    }
}

// Decisive single-8x8-block isolation: one mono block with a purely horizontal
// AC coefficient. If the IDCT orientation differs, jonboy's 64-sample block is
// the transpose of Beetle's, which this reports directly.
static void
compare_mono_block(uint32_t q_scale, const char *label)
{
    static uint8_t jout[64], bout[64];
    uint16_t stream[3];
    uint32_t words[40];
    int i, x, y, diffs = 0, tdiffs = 0, nw;

    // DC plus one AC at zig-zag index 2 (natural position (0,2): a horizontal
    // basis), so the reconstructed block varies along x but not y. q_scale 0
    // exercises the raw-coefficient path amidog uses.
    stream[0] = (uint16_t)((q_scale << 10) | (100 & 0x3FF)); // DC 100
    stream[1] = (uint16_t)((1u << 10) | (200 & 0x3FF));      // run 1 -> zig index 2
    stream[2] = 0xFE00u;

    // A nonzero quant table (all 4) so the q_scale path actually matters: the
    // DC uses quant[0] regardless of q_scale, the AC uses q_scale*quant[k].
    uint16_t qhw[64];
    for (i = 0; i < 64; i++) qhw[i] = 4 | (4 << 8);

    // jonboy: depth 1 (8-bit mono), one block.
    mdec_reset();
    mdec_write(MDEC_BASE, 3u << 29);
    for (i = 0; i < 64; i += 2)
        mdec_write(MDEC_BASE, std_scale[i] | ((uint32_t)std_scale[i + 1] << 16));
    mdec_write(MDEC_BASE, 2u << 29); // SET_QUANT, luma
    for (i = 0; i < 32; i++)
        mdec_write(MDEC_BASE, qhw[i * 2] | ((uint32_t)qhw[i * 2 + 1] << 16));
    mdec_write(MDEC_BASE, (1u << 29) | (1u << 27) | 2u);
    mdec_write(MDEC_BASE, stream[0] | ((uint32_t)stream[1] << 16));
    mdec_write(MDEC_BASE, stream[2] | ((uint32_t)0xFE00u << 16));
    for (i = 0; i < 64 && mdec_dma_out_ready(); ) {
        uint32_t w = mdec_dma_out();
        jout[i++] = (uint8_t)w;
        if (i < 64) jout[i++] = (uint8_t)(w >> 8);
        if (i < 64) jout[i++] = (uint8_t)(w >> 16);
        if (i < 64) jout[i++] = (uint8_t)(w >> 24);
    }

    // Beetle: same block.
    MDEC_Write(0, 4, 0x80000000u);
    nw = pack_words(3u << 29, std_scale, 64, words);
    beetle_command(words, nw, NULL, 0);
    nw = pack_words(2u << 29, qhw, 64, words);
    beetle_command(words, nw, NULL, 0);
    {
        uint16_t hw[3] = { stream[0], stream[1], stream[2] };
        nw = pack_words((1u << 29) | (1u << 27) | 2u, hw, 3, words);
        beetle_command(words, nw, bout, 64);
    }

    for (i = 0; i < 64; i++)
        if (jout[i] != bout[i]) diffs++;

    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            if (jout[y * 8 + x] != bout[x * 8 + y]) tdiffs++;

    printf("  mono 8x8 block (%s): %d/64 differ direct, %d/64 under transpose\n",
           label, diffs, tdiffs);

    if (diffs && !tdiffs)
        fprintf(stderr, "FAIL: jonboy IDCT output is the TRANSPOSE of Beetle "
                "(per-8x8-block orientation bug)\n");

    if (diffs) failures++;
}

int
main(void)
{
    MDEC_Power();
    build_input();

    compare_mono_block(8, "q_scale 8, quantized");
    compare_mono_block(0, "q_scale 0, raw (amidog path)");
    compare(2, "24-bit macroblock", 768);
    compare(3, "15-bit macroblock", 512);

    if (failures == 0) {
        printf("MDEC oracle: jonboy matches Beetle byte-for-byte\n");
        return 0;
    }

    fprintf(stderr, "MDEC oracle: %d failure(s)\n", failures);
    return 1;
}
