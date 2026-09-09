#include "mdec.h"

#include <string.h>

// Macroblock Decoder. The MDEC takes a bitstream of run-length-coded,
// quantized DCT coefficients and reconstructs 8x8 blocks, which it assembles
// into 16x16 macroblocks of colour pixels (or standalone monochrome blocks).
// Games use it for full-motion video and for streamed texture decompression.
//
// A command word written to the data port (0x1820) sets the operation in its
// top three bits: 1 decodes macroblocks, 2 loads the quant tables, 3 loads the
// IDCT scale table. The parameter words follow on the same port, usually fed by
// DMA channel 0. A decode's output is produced all at once when the last input
// word arrives, then drained through DMA channel 1.
//
// The decode pipeline (run-length expand and dequantize, IDCT, YCbCr to RGB)
// follows the fixed-point arithmetic of the mednafen/Beetle core so the output
// is bit-for-bit the same as that reference: dequant in a <<4 domain with a
// sign-magnitude rounding bias, a two-pass separable IDCT over a pre->>3 scale
// matrix with a 9-bit mask before the +-127 clamp, and a YCbCr conversion with
// the hardware's reduced-precision green channel.

enum {
    MDEC_CMD_DECODE = 1,
    MDEC_CMD_SET_QUANT = 2,
    MDEC_CMD_SET_SCALE = 3,
};

#define MDEC_END_OF_BLOCK 0xFE00u

// A decode command's parameter count is a 16-bit word count, so the input
// never exceeds 0xFFFF words, i.e. 0x1FFFE halfwords. The output buffer holds
// one command's decoded pixels; a 320x240 24-bit frame is ~225 KB, so 1 MB
// covers a whole frame decoded in a single command with room to spare. A
// command that would overrun it simply stops producing output.
#define MDEC_INPUT_MAX  0x20000u
#define MDEC_OUTPUT_MAX 0x100000u

// Zig-zag scatter table in the column-major order that pairs with the
// transposed IDCT matrix below (Beetle's ZigZag[64]). rl position k lands at
// natural block index zigzag_to_linear[k].
static const uint8_t zigzag_to_linear[64] = {
    0x00, 0x08, 0x01, 0x02, 0x09, 0x10, 0x18, 0x11,
    0x0a, 0x03, 0x04, 0x0b, 0x12, 0x19, 0x20, 0x28,
    0x21, 0x1a, 0x13, 0x0c, 0x05, 0x06, 0x0d, 0x14,
    0x1b, 0x22, 0x29, 0x30, 0x38, 0x31, 0x2a, 0x23,
    0x1c, 0x15, 0x0e, 0x07, 0x0f, 0x16, 0x1d, 0x24,
    0x2b, 0x32, 0x39, 0x3a, 0x33, 0x2c, 0x25, 0x1e,
    0x17, 0x1f, 0x26, 0x2d, 0x34, 0x3b, 0x3c, 0x35,
    0x2e, 0x27, 0x2f, 0x36, 0x3d, 0x3e, 0x37, 0x3f,
};

static struct {
    uint32_t command;         // the in-progress command word
    uint32_t expected;        // parameter halfwords still to collect
    uint32_t collected;       // parameter halfwords received so far

    uint16_t input[MDEC_INPUT_MAX];
    uint8_t output[MDEC_OUTPUT_MAX];
    uint32_t output_size;     // decoded bytes available
    uint32_t output_pos;      // next output byte for the read/DMA port

    uint8_t y_quant[64];
    uint8_t uv_quant[64];
    int16_t scale[64];

    uint8_t output_depth;     // 0=4bit, 1=8bit, 2=24bit, 3=15bit
    uint8_t output_signed;
    uint8_t output_bit15;
    uint8_t current_block;    // reported in the status register
    uint8_t busy;             // a command is collecting parameters
    uint8_t enable_dma_in;
    uint8_t enable_dma_out;

    uint32_t stat_decodes;    // decode commands executed since reset
    uint32_t stat_color_mb;   // colour macroblocks produced
    uint32_t stat_mono_blk;   // monochrome blocks produced
} mdec;

static int16_t
sign_extend_10(uint16_t value)
{
    return (int16_t)((int16_t)(value << 6) >> 6);
}

static int
clamp_int(int value, int lo, int hi)
{
    if (value < lo) return lo;

    if (value > hi) return hi;

    return value;
}

// Sign-extend the low count bits of v to a full int.
static int
sign_extend(int bits, int v)
{
    int shift = 32 - bits;
    return (int)(((int32_t)((uint32_t)v << shift)) >> shift);
}

// The MDEC keeps intermediate samples to 9 bits before the +-127 clamp, so a
// value outside the 9-bit window wraps before clamping. This reproduces the
// hardware's ringing on overshoot.
static int
mask9_clamp_s8(int v)
{
    v = sign_extend(9, v);
    return clamp_int(v, -128, 127);
}

// Two separable 1-D IDCT passes over the >>3 scale matrix, following Beetle's
// IDCT_1D_Pass1/Pass2. Pass one keeps int16 and transposes (out[x*8+col]); pass
// two writes back non-transposed (out[col*8+x]) through mask9_clamp_s8, so the
// two passes net one transpose, matching the hardware orientation. Rounding is
// (sum + 0x4000) >> 15.
static void
idct_block(int16_t *block)
{
    int16_t tmp[64];
    int col, x, u;

    for (col = 0; col < 8; col++) {
        const int16_t *src = block + col * 8;

        for (x = 0; x < 8; x++) {
            const int16_t *sc = mdec.scale + x * 8;
            int sum = 0;

            for (u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];

            tmp[x * 8 + col] = (int16_t)((sum + 0x4000) >> 15);
        }
    }

    for (col = 0; col < 8; col++) {
        const int16_t *src = tmp + col * 8;

        for (x = 0; x < 8; x++) {
            const int16_t *sc = mdec.scale + x * 8;
            int sum = 0;

            for (u = 0; u < 8; u++)
                sum += (int)src[u] * (int)sc[u];

            block[col * 8 + x] = (int16_t)mask9_clamp_s8((sum + 0x4000) >> 15);
        }
    }
}

// Run-length expand and dequantize one 8x8 block from the input at *pos, then
// run the IDCT over it. Returns 0 if the input is exhausted. Coefficients live
// in the <<4 fixed-point domain that feeds the >>3 IDCT matrix, with a
// sign-magnitude rounding bias on each nonzero quantized term.
//
// The DC term is quantized by quant[0]; each AC term by q_scale*quant[k]. When
// the effective quantizer is zero (a zero table entry, or q_scale zero for an
// AC term) the coefficient is taken raw, doubled. Every term scatters from its
// zig-zag scan position into the natural block order, following the mednafen/
// Beetle WriteImageData path exactly.
static int
mdec_decode_block(int16_t *block, const uint8_t *quant, uint32_t *pos, uint32_t end)
{
    uint16_t word;
    uint32_t q_scale, k;
    int ci, q, tmp;

    memset(block, 0, 64 * sizeof(int16_t));

    if (*pos >= end) return 0;

    word = mdec.input[(*pos)++];

    while (word == MDEC_END_OF_BLOCK && *pos < end)
        word = mdec.input[(*pos)++];

    q_scale = (word >> 10) & 0x3Fu;
    ci = sign_extend_10(word & 0x03FFu);
    q = (int)quant[0];
    tmp = (q != 0) ? (((ci * q) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
          : ((ci * 2) << 4);
    block[zigzag_to_linear[0]] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);

    k = 0;

    while (*pos < end && k < 63u) {
        word = mdec.input[(*pos)++];

        if (word == MDEC_END_OF_BLOCK) break;

        k += ((word >> 10) & 0x3Fu) + 1u;

        if (k >= 64u) break;

        ci = sign_extend_10(word & 0x03FFu);
        q = (int)q_scale * (int)quant[k];
        tmp = (q != 0) ? ((((ci * q) >> 3) << 4) + (ci ? (ci < 0 ? 8 : -8) : 0))
              : ((ci * 2) << 4);
        block[zigzag_to_linear[k]] = (int16_t)clamp_int(tmp, -0x4000, 0x3FFF);
    }

    idct_block(block);
    return 1;
}

// One decoded luminance sample to an output byte: clamp to signed 8-bit, then
// either keep it signed or bias to unsigned per the command's sign flag.
static uint8_t
to_output_u8(int value)
{
    value = clamp_int(value, -128, 127);

    if (mdec.output_signed) return (uint8_t)(int8_t)value;

    return (uint8_t)(value + 128);
}

static void
output_byte(uint8_t v)
{
    if (mdec.output_size < MDEC_OUTPUT_MAX)
        mdec.output[mdec.output_size++] = v;
}

// 8-bit unsigned channel to 5-bit, with the Beetle RGB555 rounding.
static int
mdec_rgb_to_555(uint8_t c)
{
    int v = (c + 4) >> 3;
    return v > 0x1F ? 0x1F : v;
}

// Emit one YCbCr pixel at the current output depth. The conversion uses the
// /256 coefficients with +0x80 rounding and the reduced-precision green mask
// (the -88*cb and -183*cr terms are masked down), then a 9-bit mask and clamp,
// then a ^0x80 back to unsigned 0..255.
static void
emit_rgb_pixel(int y, int cr, int cb)
{
    int r = mask9_clamp_s8(y + (((359 * cr) + 0x80) >> 8));
    int g = mask9_clamp_s8(y + ((((-88 * cb) & ~0x1F)
                                 + ((-183 * cr) & ~0x07) + 0x80) >> 8));
    int b = mask9_clamp_s8(y + (((454 * cb) + 0x80) >> 8));
    int ru = r ^ 0x80, gu = g ^ 0x80, bu = b ^ 0x80;

    if (mdec.output_depth == 3) {
        // 15-bit: pack RGB555, then XOR the mask bit (bit 15) and, when signed
        // output is selected, the top bit of each 5-bit channel (0x4210).
        uint16_t packed = (uint16_t)(mdec_rgb_to_555((uint8_t)ru)
                                     | (mdec_rgb_to_555((uint8_t)gu) << 5)
                                     | (mdec_rgb_to_555((uint8_t)bu) << 10));
        uint16_t pixel_xor = (uint16_t)((mdec.output_bit15 ? 0x8000u : 0u)
                                        | (mdec.output_signed ? 0x4210u : 0u));
        packed ^= pixel_xor;
        output_byte((uint8_t)packed);
        output_byte((uint8_t)(packed >> 8));
        return;
    }

    // 24-bit: one byte per channel, XOR 0x80 back to signed when requested.
    {
        uint8_t rgb_xor = mdec.output_signed ? 0x80u : 0x00u;
        output_byte((uint8_t)(ru ^ rgb_xor));
        output_byte((uint8_t)(gu ^ rgb_xor));
        output_byte((uint8_t)(bu ^ rgb_xor));
    }
}

// A monochrome 8x8 block: 8-bit writes one byte per pixel, 4-bit packs two
// pixels per byte (first pixel in the low nibble).
static void
emit_luma_block(const int16_t *blk)
{
    int i;

    if (mdec.output_depth == 1) {
        for (i = 0; i < 64; i++)
            output_byte(to_output_u8(blk[i]));

        return;
    }

    for (i = 0; i < 64; i += 2) {
        uint8_t lo = (uint8_t)(to_output_u8(blk[i]) >> 4);
        uint8_t hi = (uint8_t)(to_output_u8(blk[i + 1]) >> 4);
        output_byte((uint8_t)(lo | (hi << 4)));
    }
}

// Assemble a 16x16 colour macroblock from its six decoded blocks. The two
// chroma blocks are subsampled 2x2, so chroma index (px>>1, py>>1) covers each
// 2x2 luma quad; the four luma blocks tile the 16x16 region.
static void
emit_color_macroblock(const int16_t *crblk, const int16_t *cbblk,
                      const int16_t yblk[4][64])
{
    int px, py;

    for (py = 0; py < 16; py++) {
        for (px = 0; px < 16; px++) {
            int y_index = (py >= 8 ? 2 : 0) + (px >= 8 ? 1 : 0);
            int ly = py & 7, lx = px & 7;
            int chroma = (px >> 1) + (py >> 1) * 8;
            emit_rgb_pixel(yblk[y_index][lx + ly * 8], crblk[chroma],
                           cbblk[chroma]);
        }
    }
}

static void
execute_decode(void)
{
    uint32_t pos = 0, end = mdec.collected;

    mdec.output_size = 0;
    mdec.output_pos = 0;
    mdec.stat_decodes++;

    if (mdec.output_depth < 2) {
        int16_t yblk[64];

        while (mdec_decode_block(yblk, mdec.y_quant, &pos, end)) {
            emit_luma_block(yblk);
            mdec.stat_mono_blk++;
        }

        return;
    }

    while (pos < end) {
        int16_t crblk[64], cbblk[64], yblk[4][64];

        if (!mdec_decode_block(crblk, mdec.uv_quant, &pos, end)) break;

        if (!mdec_decode_block(cbblk, mdec.uv_quant, &pos, end)) break;

        if (!mdec_decode_block(yblk[0], mdec.y_quant, &pos, end)) break;

        if (!mdec_decode_block(yblk[1], mdec.y_quant, &pos, end)) break;

        if (!mdec_decode_block(yblk[2], mdec.y_quant, &pos, end)) break;

        if (!mdec_decode_block(yblk[3], mdec.y_quant, &pos, end)) break;

        emit_color_macroblock(crblk, cbblk, yblk);
        mdec.stat_color_mb++;
    }
}

static void
execute_command(void)
{
    uint32_t op = mdec.command >> 29;
    uint32_t i;

    if (op == MDEC_CMD_SET_QUANT) {
        // 64 bytes of luminance table, packed four per input word; the colour
        // table follows when bit 0 of the command is set, else it mirrors the
        // luminance table.
        for (i = 0; i < 64u; i++)
            mdec.y_quant[i] = (uint8_t)(mdec.input[i >> 1] >> ((i & 1u) * 8));

        if (mdec.command & 1u) {
            for (i = 0; i < 64u; i++)
                mdec.uv_quant[i] =
                    (uint8_t)(mdec.input[32u + (i >> 1)] >> ((i & 1u) * 8));
        } else {
            memcpy(mdec.uv_quant, mdec.y_quant, sizeof(mdec.uv_quant));
        }
    } else if (op == MDEC_CMD_SET_SCALE) {
        // 64 signed 16-bit entries. Store each one transposed and pre-shifted
        // >>3 so idct_block can index it directly as scale[x*8+u].
        for (i = 0; i < 64u; i++) {
            uint32_t t = ((i & 7u) << 3) | ((i >> 3) & 7u);
            mdec.scale[t] = (int16_t)((int16_t)mdec.input[i] >> 3);
        }
    } else if (op == MDEC_CMD_DECODE) {
        execute_decode();
    }

    mdec.expected = 0;
    mdec.collected = 0;
    mdec.busy = 0;
}

static void
begin_command(uint32_t value)
{
    mdec.command = value;
    mdec.output_bit15 = (uint8_t)((value >> 25) & 1u);
    mdec.output_signed = (uint8_t)((value >> 26) & 1u);
    mdec.output_depth = (uint8_t)((value >> 27) & 3u);
    mdec.current_block = 4;
    mdec.collected = 0;
    mdec.busy = 1;

    switch (value >> 29) {
    case MDEC_CMD_DECODE:
        mdec.expected = (value & 0xFFFFu) * 2u;
        break;

    case MDEC_CMD_SET_QUANT:
        mdec.expected = (value & 1u) ? 64u : 32u;
        break;

    case MDEC_CMD_SET_SCALE:
        mdec.expected = 64u;
        break;

    default:
        mdec.expected = 0;
        break;
    }

    if (mdec.expected > MDEC_INPUT_MAX)
        mdec.expected = MDEC_INPUT_MAX;

    if (mdec.expected == 0)
        mdec.busy = 0;
}

// A word written to the data port is either the next command (when idle) or a
// pair of parameter halfwords (while a command collects input). The command
// runs once its parameters are all in.
static void
write_data(uint32_t value)
{
    if (mdec.busy && mdec.collected < mdec.expected) {
        mdec.input[mdec.collected++] = (uint16_t)value;

        if (mdec.collected < mdec.expected)
            mdec.input[mdec.collected++] = (uint16_t)(value >> 16);

        if (mdec.collected >= mdec.expected)
            execute_command();

        return;
    }

    begin_command(value);
}

static uint32_t
read_data(void)
{
    uint32_t value = 0, i;

    for (i = 0; i < 4u; i++) {
        if (mdec.output_pos < mdec.output_size)
            value |= (uint32_t)mdec.output[mdec.output_pos++] << (i * 8u);
    }

    return value;
}

static uint32_t
read_status(void)
{
    uint32_t remaining = 0, status;

    if (mdec.busy && mdec.expected > mdec.collected)
        remaining = (mdec.expected - mdec.collected + 1u) / 2u;

    status = remaining ? ((remaining - 1u) & 0xFFFFu) : 0xFFFFu;
    status |= ((uint32_t)mdec.current_block & 7u) << 16;
    status |= ((uint32_t)mdec.output_bit15 & 1u) << 23;
    status |= ((uint32_t)mdec.output_signed & 1u) << 24;
    status |= ((uint32_t)mdec.output_depth & 3u) << 25;

    int out_ready = mdec.output_pos < mdec.output_size;
    int in_ready = !out_ready && (!mdec.busy || mdec.collected < mdec.expected);

    if (!in_ready) status |= 1u << 30;             // data-in FIFO full

    if (mdec.enable_dma_out && out_ready) status |= 1u << 27; // DMA1 request

    if (mdec.enable_dma_in && in_ready) status |= 1u << 28;   // DMA0 request

    if (mdec.busy) status |= 1u << 29;             // command busy

    if (!out_ready) status |= 1u << 31;            // output FIFO empty

    return status;
}

void
mdec_reset(void)
{
    uint8_t y_quant[64], uv_quant[64];
    int16_t scale[64];

    // A reset (and power-on) clears the FIFOs and state but, on hardware,
    // leaves the loaded quant and scale tables intact.
    memcpy(y_quant, mdec.y_quant, sizeof(y_quant));
    memcpy(uv_quant, mdec.uv_quant, sizeof(uv_quant));
    memcpy(scale, mdec.scale, sizeof(scale));

    memset(&mdec, 0, sizeof(mdec));

    memcpy(mdec.y_quant, y_quant, sizeof(mdec.y_quant));
    memcpy(mdec.uv_quant, uv_quant, sizeof(mdec.uv_quant));
    memcpy(mdec.scale, scale, sizeof(mdec.scale));

    mdec.output_depth = 3;
    mdec.current_block = 4;
}

uint32_t
mdec_read(uint32_t phys_addr)
{
    if (phys_addr & 4u)
        return read_status();

    return read_data();
}

void
mdec_write(uint32_t phys_addr, uint32_t value)
{
    if (phys_addr & 4u) {
        if (value & 0x80000000u)
            mdec_reset();

        mdec.enable_dma_in = (uint8_t)((value >> 30) & 1u);
        mdec.enable_dma_out = (uint8_t)((value >> 29) & 1u);
        return;
    }

    write_data(value);
}

void
mdec_dma_in(uint32_t word)
{
    write_data(word);
}

uint32_t
mdec_dma_out(void)
{
    return read_data();
}

int
mdec_dma_out_ready(void)
{
    return mdec.output_pos < mdec.output_size;
}

void
mdec_get_stats(uint32_t *decodes, uint32_t *color_macroblocks,
               uint32_t *mono_blocks)
{
    if (decodes) *decodes = mdec.stat_decodes;

    if (color_macroblocks) *color_macroblocks = mdec.stat_color_mb;

    if (mono_blocks) *mono_blocks = mdec.stat_mono_blk;
}
