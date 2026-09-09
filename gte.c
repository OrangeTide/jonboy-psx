#include "gte.h"

#include <string.h>

// Geometry Transformation Engine. This file currently implements the register
// file and the move-op accessors the CPU dispatch calls for MFC2/CFC2/MTC2/CTC2
// and LWC2/SWC2. The command ops (RTPS, NCLIP, the lighting ops, and so on) are
// stubbed and land in a follow-up.

GTE gte;

void
gte_reset(void)
{
    memset(&gte, 0, sizeof(gte));
}

// Pack two signed 16-bit halves into a 32-bit register image: low half in bits
// 0..15, high half in bits 16..31.
static uint32_t
pack16(int16_t lo, int16_t hi)
{
    return (uint32_t)(uint16_t)lo | ((uint32_t)(uint16_t)hi << 16);
}

// Saturate a value into the 0..0x1F range for the 5-bit colour conversion that
// backs the IRGB and ORGB register reads.
static int
sat_rgb5(int v)
{
    if (v < 0) return 0;
    if (v > 0x1F) return 0x1F;
    return v;
}

// The colour-conversion value read back from IRGB (reg 28) and ORGB (reg 29):
// IR1/IR2/IR3, each divided by 0x80 and clamped to five bits, packed r,g,b.
static uint32_t
orgb_value(void)
{
    int r = sat_rgb5(gte.ir[1] >> 7);
    int g = sat_rgb5(gte.ir[2] >> 7);
    int b = sat_rgb5(gte.ir[3] >> 7);
    return (uint32_t)(r | (g << 5) | (b << 10));
}

// Leading-zero-count result for LZCR: the number of leading zero bits in LZCS
// when it is positive, or leading one bits when it is negative. Range 1..32.
static int32_t
leading_count(uint32_t v)
{
    uint32_t bits = (v & 0x80000000u) ? ~v : v;
    int n = 0;

    // Count the leading zero bits of bits (an all-zero word counts as 32).
    for (int i = 31; i >= 0; i--) {
        if (bits & (1u << i)) break;
        n++;
    }

    return n;
}

uint32_t
gte_read_data(uint32_t reg)
{
    switch (reg & 0x1F) {
    case 0:  return pack16(gte.v[0][0], gte.v[0][1]);
    case 1:  return (uint32_t)(int32_t)gte.v[0][2];
    case 2:  return pack16(gte.v[1][0], gte.v[1][1]);
    case 3:  return (uint32_t)(int32_t)gte.v[1][2];
    case 4:  return pack16(gte.v[2][0], gte.v[2][1]);
    case 5:  return (uint32_t)(int32_t)gte.v[2][2];
    case 6:  return (uint32_t)gte.rgbc[0] | ((uint32_t)gte.rgbc[1] << 8)
                    | ((uint32_t)gte.rgbc[2] << 16) | ((uint32_t)gte.rgbc[3] << 24);
    case 7:  return (uint32_t)gte.otz;
    case 8:  return (uint32_t)(int32_t)gte.ir[0];
    case 9:  return (uint32_t)(int32_t)gte.ir[1];
    case 10: return (uint32_t)(int32_t)gte.ir[2];
    case 11: return (uint32_t)(int32_t)gte.ir[3];
    case 12: return pack16(gte.sxy[0][0], gte.sxy[0][1]);
    case 13: return pack16(gte.sxy[1][0], gte.sxy[1][1]);
    case 14: // SXY2 and SXYP (15) both read the top FIFO entry.
    case 15: return pack16(gte.sxy[2][0], gte.sxy[2][1]);
    case 16: return (uint32_t)gte.sz[0];
    case 17: return (uint32_t)gte.sz[1];
    case 18: return (uint32_t)gte.sz[2];
    case 19: return (uint32_t)gte.sz[3];
    case 20: return (uint32_t)gte.rgb[0][0] | ((uint32_t)gte.rgb[0][1] << 8)
                    | ((uint32_t)gte.rgb[0][2] << 16) | ((uint32_t)gte.rgb[0][3] << 24);
    case 21: return (uint32_t)gte.rgb[1][0] | ((uint32_t)gte.rgb[1][1] << 8)
                    | ((uint32_t)gte.rgb[1][2] << 16) | ((uint32_t)gte.rgb[1][3] << 24);
    case 22: return (uint32_t)gte.rgb[2][0] | ((uint32_t)gte.rgb[2][1] << 8)
                    | ((uint32_t)gte.rgb[2][2] << 16) | ((uint32_t)gte.rgb[2][3] << 24);
    case 23: return gte.res1;
    case 24: return (uint32_t)gte.mac[0];
    case 25: return (uint32_t)gte.mac[1];
    case 26: return (uint32_t)gte.mac[2];
    case 27: return (uint32_t)gte.mac[3];
    case 28: // IRGB and ORGB both read the 5-bit colour conversion of IR1..IR3.
    case 29: return orgb_value();
    case 30: return gte.lzcs;
    case 31: return (uint32_t)gte.lzcr;
    }

    return 0;
}

void
gte_write_data(uint32_t reg, uint32_t value)
{
    switch (reg & 0x1F) {
    case 0:  gte.v[0][0] = (int16_t)value; gte.v[0][1] = (int16_t)(value >> 16); break;
    case 1:  gte.v[0][2] = (int16_t)value; break;
    case 2:  gte.v[1][0] = (int16_t)value; gte.v[1][1] = (int16_t)(value >> 16); break;
    case 3:  gte.v[1][2] = (int16_t)value; break;
    case 4:  gte.v[2][0] = (int16_t)value; gte.v[2][1] = (int16_t)(value >> 16); break;
    case 5:  gte.v[2][2] = (int16_t)value; break;
    case 6:
        gte.rgbc[0] = (uint8_t)value;        gte.rgbc[1] = (uint8_t)(value >> 8);
        gte.rgbc[2] = (uint8_t)(value >> 16); gte.rgbc[3] = (uint8_t)(value >> 24);
        break;
    case 7:  gte.otz = (uint16_t)value; break;
    case 8:  gte.ir[0] = (int16_t)value; break;
    case 9:  gte.ir[1] = (int16_t)value; break;
    case 10: gte.ir[2] = (int16_t)value; break;
    case 11: gte.ir[3] = (int16_t)value; break;
    case 12: gte.sxy[0][0] = (int16_t)value; gte.sxy[0][1] = (int16_t)(value >> 16); break;
    case 13: gte.sxy[1][0] = (int16_t)value; gte.sxy[1][1] = (int16_t)(value >> 16); break;
    case 14: gte.sxy[2][0] = (int16_t)value; gte.sxy[2][1] = (int16_t)(value >> 16); break;
    case 15:
        // SXYP: a write pushes the screen-coordinate FIFO (SXY1->SXY0,
        // SXY2->SXY1, new value into SXY2).
        gte.sxy[0][0] = gte.sxy[1][0]; gte.sxy[0][1] = gte.sxy[1][1];
        gte.sxy[1][0] = gte.sxy[2][0]; gte.sxy[1][1] = gte.sxy[2][1];
        gte.sxy[2][0] = (int16_t)value; gte.sxy[2][1] = (int16_t)(value >> 16);
        break;
    case 16: gte.sz[0] = (uint16_t)value; break;
    case 17: gte.sz[1] = (uint16_t)value; break;
    case 18: gte.sz[2] = (uint16_t)value; break;
    case 19: gte.sz[3] = (uint16_t)value; break;
    case 20:
        gte.rgb[0][0] = (uint8_t)value;        gte.rgb[0][1] = (uint8_t)(value >> 8);
        gte.rgb[0][2] = (uint8_t)(value >> 16); gte.rgb[0][3] = (uint8_t)(value >> 24);
        break;
    case 21:
        gte.rgb[1][0] = (uint8_t)value;        gte.rgb[1][1] = (uint8_t)(value >> 8);
        gte.rgb[1][2] = (uint8_t)(value >> 16); gte.rgb[1][3] = (uint8_t)(value >> 24);
        break;
    case 22:
        gte.rgb[2][0] = (uint8_t)value;        gte.rgb[2][1] = (uint8_t)(value >> 8);
        gte.rgb[2][2] = (uint8_t)(value >> 16); gte.rgb[2][3] = (uint8_t)(value >> 24);
        break;
    case 23: gte.res1 = value; break;
    case 24: gte.mac[0] = (int32_t)value; break;
    case 25: gte.mac[1] = (int32_t)value; break;
    case 26: gte.mac[2] = (int32_t)value; break;
    case 27: gte.mac[3] = (int32_t)value; break;
    case 28:
        // IRGB: a write splits a 15-bit colour into IR1..IR3, each field scaled
        // back up by 0x80.
        gte.ir[1] = (int16_t)((value & 0x1F) << 7);
        gte.ir[2] = (int16_t)(((value >> 5) & 0x1F) << 7);
        gte.ir[3] = (int16_t)(((value >> 10) & 0x1F) << 7);
        break;
    case 29: break; // ORGB is read-only.
    case 30: gte.lzcs = value; gte.lzcr = leading_count(value); break;
    case 31: break; // LZCR is read-only.
    }
}

uint32_t
gte_read_control(uint32_t reg)
{
    switch (reg & 0x1F) {
    case 0:  return pack16(gte.rt[0][0], gte.rt[0][1]);
    case 1:  return pack16(gte.rt[0][2], gte.rt[1][0]);
    case 2:  return pack16(gte.rt[1][1], gte.rt[1][2]);
    case 3:  return pack16(gte.rt[2][0], gte.rt[2][1]);
    case 4:  return (uint32_t)(int32_t)gte.rt[2][2];
    case 5:  return (uint32_t)gte.tr[0];
    case 6:  return (uint32_t)gte.tr[1];
    case 7:  return (uint32_t)gte.tr[2];
    case 8:  return pack16(gte.llm[0][0], gte.llm[0][1]);
    case 9:  return pack16(gte.llm[0][2], gte.llm[1][0]);
    case 10: return pack16(gte.llm[1][1], gte.llm[1][2]);
    case 11: return pack16(gte.llm[2][0], gte.llm[2][1]);
    case 12: return (uint32_t)(int32_t)gte.llm[2][2];
    case 13: return (uint32_t)gte.bk[0];
    case 14: return (uint32_t)gte.bk[1];
    case 15: return (uint32_t)gte.bk[2];
    case 16: return pack16(gte.lcm[0][0], gte.lcm[0][1]);
    case 17: return pack16(gte.lcm[0][2], gte.lcm[1][0]);
    case 18: return pack16(gte.lcm[1][1], gte.lcm[1][2]);
    case 19: return pack16(gte.lcm[2][0], gte.lcm[2][1]);
    case 20: return (uint32_t)(int32_t)gte.lcm[2][2];
    case 21: return (uint32_t)gte.fc[0];
    case 22: return (uint32_t)gte.fc[1];
    case 23: return (uint32_t)gte.fc[2];
    case 24: return (uint32_t)gte.ofx;
    case 25: return (uint32_t)gte.ofy;
    // H reads back sign-extended from 16 bits even though it is used unsigned,
    // a documented hardware quirk.
    case 26: return (uint32_t)(int32_t)(int16_t)gte.h;
    case 27: return (uint32_t)(int32_t)gte.dqa;
    case 28: return (uint32_t)gte.dqb;
    case 29: return (uint32_t)(int32_t)gte.zsf3;
    case 30: return (uint32_t)(int32_t)gte.zsf4;
    case 31: return gte.flag;
    }

    return 0;
}

// FLAG bit 31 is the logical OR of the significant error bits (30..23 and
// 18..13), recomputed whenever FLAG changes.
static void
recalc_flag_msb(void)
{
    if (gte.flag & 0x7F87E000u)
        gte.flag |= 0x80000000u;
    else
        gte.flag &= ~0x80000000u;
}

void
gte_write_control(uint32_t reg, uint32_t value)
{
    switch (reg & 0x1F) {
    case 0:  gte.rt[0][0] = (int16_t)value; gte.rt[0][1] = (int16_t)(value >> 16); break;
    case 1:  gte.rt[0][2] = (int16_t)value; gte.rt[1][0] = (int16_t)(value >> 16); break;
    case 2:  gte.rt[1][1] = (int16_t)value; gte.rt[1][2] = (int16_t)(value >> 16); break;
    case 3:  gte.rt[2][0] = (int16_t)value; gte.rt[2][1] = (int16_t)(value >> 16); break;
    case 4:  gte.rt[2][2] = (int16_t)value; break;
    case 5:  gte.tr[0] = (int32_t)value; break;
    case 6:  gte.tr[1] = (int32_t)value; break;
    case 7:  gte.tr[2] = (int32_t)value; break;
    case 8:  gte.llm[0][0] = (int16_t)value; gte.llm[0][1] = (int16_t)(value >> 16); break;
    case 9:  gte.llm[0][2] = (int16_t)value; gte.llm[1][0] = (int16_t)(value >> 16); break;
    case 10: gte.llm[1][1] = (int16_t)value; gte.llm[1][2] = (int16_t)(value >> 16); break;
    case 11: gte.llm[2][0] = (int16_t)value; gte.llm[2][1] = (int16_t)(value >> 16); break;
    case 12: gte.llm[2][2] = (int16_t)value; break;
    case 13: gte.bk[0] = (int32_t)value; break;
    case 14: gte.bk[1] = (int32_t)value; break;
    case 15: gte.bk[2] = (int32_t)value; break;
    case 16: gte.lcm[0][0] = (int16_t)value; gte.lcm[0][1] = (int16_t)(value >> 16); break;
    case 17: gte.lcm[0][2] = (int16_t)value; gte.lcm[1][0] = (int16_t)(value >> 16); break;
    case 18: gte.lcm[1][1] = (int16_t)value; gte.lcm[1][2] = (int16_t)(value >> 16); break;
    case 19: gte.lcm[2][0] = (int16_t)value; gte.lcm[2][1] = (int16_t)(value >> 16); break;
    case 20: gte.lcm[2][2] = (int16_t)value; break;
    case 21: gte.fc[0] = (int32_t)value; break;
    case 22: gte.fc[1] = (int32_t)value; break;
    case 23: gte.fc[2] = (int32_t)value; break;
    case 24: gte.ofx = (int32_t)value; break;
    case 25: gte.ofy = (int32_t)value; break;
    case 26: gte.h = (uint16_t)value; break;
    case 27: gte.dqa = (int16_t)value; break;
    case 28: gte.dqb = (int32_t)value; break;
    case 29: gte.zsf3 = (int16_t)value; break;
    case 30: gte.zsf4 = (int16_t)value; break;
    case 31:
        gte.flag = value & 0x7FFFF000u;
        recalc_flag_msb();
        break;
    }
}

// ===========================================================================
// Command ops
//
// The command instructions run the fixed-point geometry math. They share a
// MAC/IR pipeline: products accumulate in a 44-bit accumulator, each result is
// shifted by the per-op shift factor (sf) into a 32-bit MAC register, and the
// MAC value is then clamped into a 16-bit IR register (the lm flag forces the
// clamp floor to 0). The FLAG register collects overflow and saturation bits as
// the op runs. The arithmetic follows nocash psx-spx and the pcsx-redux/mednafen
// reference behavior.
// ===========================================================================

// FLAG bits (excluding bit 31, the error summary, recomputed at the end of each
// command by recalc_flag_msb).
#define FLAG_MAC1_POS  (1u << 30)
#define FLAG_MAC2_POS  (1u << 29)
#define FLAG_MAC3_POS  (1u << 28)
#define FLAG_MAC1_NEG  (1u << 27)
#define FLAG_MAC2_NEG  (1u << 26)
#define FLAG_MAC3_NEG  (1u << 25)
#define FLAG_IR1_SAT   (1u << 24)
#define FLAG_IR2_SAT   (1u << 23)
#define FLAG_IR3_SAT   (1u << 22)
#define FLAG_COLOR_R   (1u << 21)
#define FLAG_COLOR_G   (1u << 20)
#define FLAG_COLOR_B   (1u << 19)
#define FLAG_SZ_SAT    (1u << 18)
#define FLAG_DIV_OVER  (1u << 17)
#define FLAG_MAC0_POS  (1u << 16)
#define FLAG_MAC0_NEG  (1u << 15)
#define FLAG_SX_SAT    (1u << 14)
#define FLAG_SY_SAT    (1u << 13)
#define FLAG_IR0_SAT   (1u << 12)

// 44-bit accumulator with sticky positive/negative overflow tracking. Each add
// sign-extends the running sum back to 44 bits and records a wrap, which feeds
// the MAC overflow FLAG bits.
typedef struct {
    int64_t value;
    int pos_ovf;
    int neg_ovf;
} acc44;

static acc44
acc_from(int64_t v)
{
    acc44 a;
    a.value = v;
    a.pos_ovf = v > 0x7FFFFFFFFFFLL;
    a.neg_ovf = v < -0x80000000000LL;
    return a;
}

static acc44
acc_add(acc44 a, int64_t rhs)
{
    int64_t result = ((a.value + rhs) << 20) >> 20; // sign-extend to 44 bits
    acc44 r;
    r.value = result;
    r.pos_ovf = a.pos_ovf || (result < 0 && a.value >= 0 && rhs >= 0);
    r.neg_ovf = a.neg_ovf || (result >= 0 && a.value < 0 && rhs < 0);
    return r;
}

static int64_t
shift_sf(int64_t v, int sf)
{
    return sf ? (v >> 12) : v;
}

// MAC1/2/3 writeback: flag the 44-bit overflow, then shift into the 32-bit MAC.
static int32_t
mac_store(acc44 a, int sf, uint32_t pos_flag, uint32_t neg_flag)
{
    if (a.pos_ovf) gte.flag |= pos_flag;
    if (a.neg_ovf) gte.flag |= neg_flag;
    return (int32_t)shift_sf(a.value, sf);
}

static int32_t A1(acc44 a, int sf) { return mac_store(a, sf, FLAG_MAC1_POS, FLAG_MAC1_NEG); }
static int32_t A2(acc44 a, int sf) { return mac_store(a, sf, FLAG_MAC2_POS, FLAG_MAC2_NEG); }

// A3 also exposes the raw (pre-shift) accumulator, which several ops reuse for
// the IR3 and SZ clamps.
static int32_t
A3(acc44 a, int sf, int64_t *raw)
{
    if (raw) *raw = a.value;
    return mac_store(a, sf, FLAG_MAC3_POS, FLAG_MAC3_NEG);
}

// MAC0: 32-bit overflow flags, no shift.
static int64_t
F0(int64_t a)
{
    if (a > 0x7FFFFFFFLL) gte.flag |= FLAG_MAC0_POS;
    if (a < -0x80000000LL) gte.flag |= FLAG_MAC0_NEG;
    return a;
}

static int32_t
clamp_ir(int32_t v, int lm, uint32_t flag)
{
    int32_t min = lm ? 0 : -0x8000;
    if (v > 0x7FFF) { gte.flag |= flag; return 0x7FFF; }
    if (v < min)    { gte.flag |= flag; return min; }
    return v;
}

static int16_t limB1(int32_t v, int lm) { return (int16_t)clamp_ir(v, lm, FLAG_IR1_SAT); }
static int16_t limB2(int32_t v, int lm) { return (int16_t)clamp_ir(v, lm, FLAG_IR2_SAT); }
static int16_t limB3(int32_t v, int lm) { return (int16_t)clamp_ir(v, lm, FLAG_IR3_SAT); }

// IR3 clamp for RTPS: the saturation flag is decided from the always->>12 value
// even when the op runs with sf == 0.
static int16_t
limB3_sf(int64_t raw_mac3, int sf, int lm)
{
    int32_t value_sf = (int32_t)shift_sf(raw_mac3, sf);
    int32_t value12  = (int32_t)(raw_mac3 >> 12);
    int32_t min = lm ? 0 : -0x8000;

    if (value12 < -0x8000 || value12 > 0x7FFF) gte.flag |= FLAG_IR3_SAT;
    if (value_sf > 0x7FFF) return 0x7FFF;
    if (value_sf < min)    return (int16_t)min;
    return (int16_t)value_sf;
}

static uint8_t
limC(int32_t v, uint32_t flag)
{
    if (v > 0xFF) { gte.flag |= flag; return 0xFF; }
    if (v < 0)    { gte.flag |= flag; return 0; }
    return (uint8_t)v;
}

// SZ/OTZ clamp: always shift >>12, clamp to unsigned 16-bit.
static uint16_t
limD(int64_t a)
{
    int64_t v = a >> 12;
    if (v > 0xFFFF) { gte.flag |= FLAG_SZ_SAT; return 0xFFFF; }
    if (v < 0)      { gte.flag |= FLAG_SZ_SAT; return 0; }
    return (uint16_t)v;
}

static int32_t
limG1(int64_t a)
{
    if (a > 0x3FF)  { gte.flag |= FLAG_SX_SAT; return 0x3FF; }
    if (a < -0x400) { gte.flag |= FLAG_SX_SAT; return -0x400; }
    return (int32_t)a;
}

static int32_t
limG2(int64_t a)
{
    if (a > 0x3FF)  { gte.flag |= FLAG_SY_SAT; return 0x3FF; }
    if (a < -0x400) { gte.flag |= FLAG_SY_SAT; return -0x400; }
    return (int32_t)a;
}

// IR0 clamp for the RTPS depth-cue: >>12, clamp to 0..0x1000.
static int16_t
limH(int64_t raw_mac0)
{
    int64_t v = raw_mac0 >> 12;
    if (v < 0)      { gte.flag |= FLAG_IR0_SAT; return 0; }
    if (v > 0x1000) { gte.flag |= FLAG_IR0_SAT; return 0x1000; }
    return (int16_t)v;
}

// ---- RTPS division: H / SZ3 via the hardware Newton-Raphson UNR table ----

static uint8_t unr_table[257];
static int unr_ready;

static void
unr_init(void)
{
    for (int i = 0; i < 257; i++) {
        int v = ((0x40000 / (i + 0x100)) + 1) / 2 - 0x101;
        unr_table[i] = (uint8_t)(v < 0 ? 0 : v);
    }
    unr_ready = 1;
}

static int
clz16(uint16_t d)
{
    int n = 0;
    while (n < 16 && !(d & 0x8000)) {
        d <<= 1;
        n++;
    }
    return n;
}

static uint32_t
gte_divide(uint16_t numerator, uint16_t denominator)
{
    if (!unr_ready) unr_init();

    if (numerator >= (uint32_t)denominator * 2) {
        gte.flag |= FLAG_DIV_OVER;
        return 0x1FFFF;
    }

    int shift = clz16(denominator);
    int r1 = (denominator << shift) & 0x7FFF;
    int r2 = unr_table[(r1 + 0x40) >> 7] + 0x101;
    int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1FFFF;
    uint32_t reciprocal = ((r2 * r3) + 0x80) >> 8;
    uint64_t result = (((uint64_t)reciprocal * ((uint32_t)numerator << shift)) + 0x8000) >> 16;

    return result < 0x1FFFF ? (uint32_t)result : 0x1FFFF;
}

// ---- FIFO pushes ----

static void
push_z(uint16_t z)
{
    gte.sz[0] = gte.sz[1];
    gte.sz[1] = gte.sz[2];
    gte.sz[2] = gte.sz[3];
    gte.sz[3] = z;
}

static void
push_color(void)
{
    memcpy(gte.rgb[0], gte.rgb[1], 4);
    memcpy(gte.rgb[1], gte.rgb[2], 4);
    gte.rgb[2][3] = gte.rgbc[3]; // code
    gte.rgb[2][0] = limC(gte.mac[1] >> 4, FLAG_COLOR_R);
    gte.rgb[2][1] = limC(gte.mac[2] >> 4, FLAG_COLOR_G);
    gte.rgb[2][2] = limC(gte.mac[3] >> 4, FLAG_COLOR_B);
}

// ---- operand selectors ----

// Matrix element (mx 0=RT, 1=LLM, 2=LCM). mx 3 selects the documented "garbage"
// matrix built from RGBC, IR0, and two rotation-matrix entries.
static int32_t
matrix_element(int mx, int row, int col)
{
    if (mx == 0) return gte.rt[row][col];
    if (mx == 1) return gte.llm[row][col];
    if (mx == 2) return gte.lcm[row][col];

    switch (row * 3 + col) {
    case 0:  return -((int32_t)gte.rgbc[0] << 4);
    case 1:  return (int32_t)gte.rgbc[0] << 4;
    case 2:  return gte.ir[0];
    case 3: case 4: case 5: return gte.rt[0][2]; // R13
    default: return gte.rt[1][1];                // R22
    }
}

// Vector (v 0..2 = V0..V2, v 3 = the IR1..IR3 vector). comp 0=x, 1=y, 2=z.
static int16_t
vector_c(int v, int comp)
{
    if (v < 3) return gte.v[v][comp];
    return gte.ir[comp + 1];
}

// Control vector (cv 0=TR, 1=BK, 2=FC, 3=zero).
static int64_t
control_vector(int cv, int comp)
{
    if (cv == 0) return gte.tr[comp];
    if (cv == 1) return gte.bk[comp];
    if (cv == 2) return gte.fc[comp];
    return 0;
}

// ---- shared pipeline stages ----

// General matrix-vector multiply: MAC = CV + MX * V, then IR = clamp(MAC). When
// cv selects the far colour (cv == 2) the hardware computes columns 1 and 2 for
// the result but uses only column 0 for FLAG side effects, then discards it (the
// documented FC bug), so the result omits the first matrix column.
static void
mat_vec(int sf, int lm, int mx, int v, int cv, int64_t *raw_mac3)
{
    int16_t vx = vector_c(v, 0), vy = vector_c(v, 1), vz = vector_c(v, 2);
    int64_t dummy;

    if (raw_mac3 == NULL) raw_mac3 = &dummy;

    if (cv == 2) {
        gte.mac[1] = A1(acc_add(acc_from((int64_t)matrix_element(mx, 0, 1) * vy),
                                (int64_t)matrix_element(mx, 0, 2) * vz), sf);
        gte.mac[2] = A2(acc_add(acc_from((int64_t)matrix_element(mx, 1, 1) * vy),
                                (int64_t)matrix_element(mx, 1, 2) * vz), sf);
        gte.mac[3] = A3(acc_add(acc_from((int64_t)matrix_element(mx, 2, 1) * vy),
                                (int64_t)matrix_element(mx, 2, 2) * vz), sf, raw_mac3);
        // Column 0: FLAG side effects only.
        limB1(A1(acc_add(acc_from(control_vector(cv, 0) << 12),
                         (int64_t)matrix_element(mx, 0, 0) * vx), sf), 0);
        limB2(A2(acc_add(acc_from(control_vector(cv, 1) << 12),
                         (int64_t)matrix_element(mx, 1, 0) * vx), sf), 0);
        limB3(A3(acc_add(acc_from(control_vector(cv, 2) << 12),
                         (int64_t)matrix_element(mx, 2, 0) * vx), sf, NULL), 0);
    } else {
        gte.mac[1] = A1(acc_add(acc_add(acc_add(acc_from(control_vector(cv, 0) << 12),
                        (int64_t)matrix_element(mx, 0, 0) * vx),
                        (int64_t)matrix_element(mx, 0, 1) * vy),
                        (int64_t)matrix_element(mx, 0, 2) * vz), sf);
        gte.mac[2] = A2(acc_add(acc_add(acc_add(acc_from(control_vector(cv, 1) << 12),
                        (int64_t)matrix_element(mx, 1, 0) * vx),
                        (int64_t)matrix_element(mx, 1, 1) * vy),
                        (int64_t)matrix_element(mx, 1, 2) * vz), sf);
        gte.mac[3] = A3(acc_add(acc_add(acc_add(acc_from(control_vector(cv, 2) << 12),
                        (int64_t)matrix_element(mx, 2, 0) * vx),
                        (int64_t)matrix_element(mx, 2, 1) * vy),
                        (int64_t)matrix_element(mx, 2, 2) * vz), sf, raw_mac3);
    }

    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
}

// Depth-cue interpolation toward the far colour FC, shared by the colour ops.
static void
depth_cue(int sf, int lm, int64_t in_r, int64_t in_g, int64_t in_b)
{
    int32_t fr = limB1(A1(acc_from(((int64_t)gte.fc[0] << 12) - in_r), sf), 0);
    gte.mac[1] = A1(acc_from(in_r + (int64_t)gte.ir[0] * fr), sf);
    int32_t fg = limB2(A2(acc_from(((int64_t)gte.fc[1] << 12) - in_g), sf), 0);
    gte.mac[2] = A2(acc_from(in_g + (int64_t)gte.ir[0] * fg), sf);
    int64_t raw_mac3;
    int32_t fb = limB3(A3(acc_from(((int64_t)gte.fc[2] << 12) - in_b), sf, NULL), 0);
    gte.mac[3] = A3(acc_from(in_b + (int64_t)gte.ir[0] * fb), sf, &raw_mac3);

    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
}

static void
depth_cue_color(int sf, int lm)
{
    depth_cue(sf, lm,
              (int64_t)(gte.rgbc[0] << 4) * gte.ir[1],
              (int64_t)(gte.rgbc[1] << 4) * gte.ir[2],
              (int64_t)(gte.rgbc[2] << 4) * gte.ir[3]);
}

static void
color_apply(int sf, int lm)
{
    gte.mac[1] = A1(acc_from((int64_t)(gte.rgbc[0] << 4) * gte.ir[1]), sf);
    gte.mac[2] = A2(acc_from((int64_t)(gte.rgbc[1] << 4) * gte.ir[2]), sf);
    gte.mac[3] = A3(acc_from((int64_t)(gte.rgbc[2] << 4) * gte.ir[3]), sf, NULL);
    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
}

// ---- the individual ops ----

// RTPS perspective transform for vertex v. When last is set (the final vertex
// of an RTPS/RTPT), it also computes the depth-cue interpolation factor IR0.
static void
rtps(int sf, int lm, int v, int last)
{
    int16_t vx = vector_c(v, 0), vy = vector_c(v, 1), vz = vector_c(v, 2);
    int64_t raw_mac3;

    gte.mac[1] = A1(acc_add(acc_add(acc_add(acc_from((int64_t)gte.tr[0] << 12),
                    (int64_t)gte.rt[0][0] * vx), (int64_t)gte.rt[0][1] * vy),
                    (int64_t)gte.rt[0][2] * vz), sf);
    gte.mac[2] = A2(acc_add(acc_add(acc_add(acc_from((int64_t)gte.tr[1] << 12),
                    (int64_t)gte.rt[1][0] * vx), (int64_t)gte.rt[1][1] * vy),
                    (int64_t)gte.rt[1][2] * vz), sf);
    gte.mac[3] = A3(acc_add(acc_add(acc_add(acc_from((int64_t)gte.tr[2] << 12),
                    (int64_t)gte.rt[2][0] * vx), (int64_t)gte.rt[2][1] * vy),
                    (int64_t)gte.rt[2][2] * vz), sf, &raw_mac3);

    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3_sf(raw_mac3, sf, lm);

    push_z(limD(raw_mac3));

    uint32_t h_over_sz3 = gte_divide(gte.h, gte.sz[3]);

    gte.sxy[0][0] = gte.sxy[1][0]; gte.sxy[0][1] = gte.sxy[1][1];
    gte.sxy[1][0] = gte.sxy[2][0]; gte.sxy[1][1] = gte.sxy[2][1];

    int64_t sx = (int64_t)gte.ofx + (int64_t)gte.ir[1] * h_over_sz3;
    int64_t sy = (int64_t)gte.ofy + (int64_t)gte.ir[2] * h_over_sz3;
    gte.sxy[2][0] = (int16_t)limG1(F0(sx) >> 16);
    gte.sxy[2][1] = (int16_t)limG2(F0(sy) >> 16);

    if (last) {
        int64_t raw_mac0 = (int64_t)gte.dqb + (int64_t)gte.dqa * h_over_sz3;
        gte.mac[0] = (int32_t)F0(raw_mac0);
        gte.ir[0] = limH(raw_mac0);
    }
}

static void
nclip(void)
{
    int64_t v =
        (int64_t)gte.sxy[0][0] * gte.sxy[1][1] + (int64_t)gte.sxy[1][0] * gte.sxy[2][1]
        + (int64_t)gte.sxy[2][0] * gte.sxy[0][1] - (int64_t)gte.sxy[0][0] * gte.sxy[2][1]
        - (int64_t)gte.sxy[1][0] * gte.sxy[0][1] - (int64_t)gte.sxy[2][0] * gte.sxy[1][1];
    gte.mac[0] = (int32_t)F0(v);
}

static void
avsz3(void)
{
    int64_t raw = (int64_t)gte.zsf3 * gte.sz[1] + (int64_t)gte.zsf3 * gte.sz[2]
                  + (int64_t)gte.zsf3 * gte.sz[3];
    gte.mac[0] = (int32_t)F0(raw);
    gte.otz = limD(raw);
}

static void
avsz4(void)
{
    int64_t raw = (int64_t)gte.zsf4 * gte.sz[0] + (int64_t)gte.zsf4 * gte.sz[1]
                  + (int64_t)gte.zsf4 * gte.sz[2] + (int64_t)gte.zsf4 * gte.sz[3];
    gte.mac[0] = (int32_t)F0(raw);
    gte.otz = limD(raw);
}

static void
op_cross(int sf, int lm)
{
    int16_t d1 = gte.rt[0][0], d2 = gte.rt[1][1], d3 = gte.rt[2][2];
    gte.mac[1] = A1(acc_from((int64_t)d2 * gte.ir[3] - (int64_t)d3 * gte.ir[2]), sf);
    gte.mac[2] = A2(acc_from((int64_t)d3 * gte.ir[1] - (int64_t)d1 * gte.ir[3]), sf);
    gte.mac[3] = A3(acc_from((int64_t)d1 * gte.ir[2] - (int64_t)d2 * gte.ir[1]), sf, NULL);
    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
}

static void
sqr(int sf, int lm)
{
    gte.mac[1] = A1(acc_from((int64_t)gte.ir[1] * gte.ir[1]), sf);
    gte.mac[2] = A2(acc_from((int64_t)gte.ir[2] * gte.ir[2]), sf);
    gte.mac[3] = A3(acc_from((int64_t)gte.ir[3] * gte.ir[3]), sf, NULL);
    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
}

static void
gpf(int sf, int lm)
{
    gte.mac[1] = A1(acc_from((int64_t)gte.ir[0] * gte.ir[1]), sf);
    gte.mac[2] = A2(acc_from((int64_t)gte.ir[0] * gte.ir[2]), sf);
    gte.mac[3] = A3(acc_from((int64_t)gte.ir[0] * gte.ir[3]), sf, NULL);
    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
    push_color();
}

static void
gpl(int sf, int lm)
{
    int64_t m1 = sf ? ((int64_t)gte.mac[1] << 12) : gte.mac[1];
    int64_t m2 = sf ? ((int64_t)gte.mac[2] << 12) : gte.mac[2];
    int64_t m3 = sf ? ((int64_t)gte.mac[3] << 12) : gte.mac[3];
    gte.mac[1] = A1(acc_from(m1 + (int64_t)gte.ir[0] * gte.ir[1]), sf);
    gte.mac[2] = A2(acc_from(m2 + (int64_t)gte.ir[0] * gte.ir[2]), sf);
    gte.mac[3] = A3(acc_from(m3 + (int64_t)gte.ir[0] * gte.ir[3]), sf, NULL);
    gte.ir[1] = limB1(gte.mac[1], lm);
    gte.ir[2] = limB2(gte.mac[2], lm);
    gte.ir[3] = limB3(gte.mac[3], lm);
    push_color();
}

// NCDS/NCDT (normal colour depth-cue), NCCS/NCCT (normal colour), NCS/NCT
// (normal colour only) share a light transform followed by the colour matrix.
static void
ncds_core(int sf, int lm, int v)
{
    mat_vec(sf, lm, 1, v, 3, NULL); // L * V -> IR (light)
    mat_vec(sf, lm, 2, 3, 1, NULL); // BK + LCM * IR -> IR (colour)
    depth_cue_color(sf, lm);
    push_color();
}

static void
nccs_core(int sf, int lm, int v)
{
    mat_vec(sf, lm, 1, v, 3, NULL);
    mat_vec(sf, lm, 2, 3, 1, NULL);
    color_apply(sf, lm);
    push_color();
}

static void
ncs_core(int sf, int lm, int v)
{
    mat_vec(sf, lm, 1, v, 3, NULL);
    mat_vec(sf, lm, 2, 3, 1, NULL);
    push_color();
}

void
gte_execute(uint32_t instruction)
{
    int sf = (instruction >> 19) & 1;
    int lm = (instruction >> 10) & 1;

    gte.flag = 0;

    switch (instruction & 0x3Fu) {
    case 0x01: rtps(sf, lm, 0, 1); break;
    case 0x06: nclip(); break;
    case 0x0C: op_cross(sf, lm); break;
    case 0x10: // DPCS
        depth_cue(sf, lm, (int64_t)gte.rgbc[0] << 16, (int64_t)gte.rgbc[1] << 16,
                  (int64_t)gte.rgbc[2] << 16);
        push_color();
        break;
    case 0x11: // INTPL
        depth_cue(sf, lm, (int64_t)gte.ir[1] << 12, (int64_t)gte.ir[2] << 12,
                  (int64_t)gte.ir[3] << 12);
        push_color();
        break;
    case 0x12: { // MVMVA
        int mx = (instruction >> 17) & 3;
        int v  = (instruction >> 15) & 3;
        int cv = (instruction >> 13) & 3;
        mat_vec(sf, lm, mx, v, cv, NULL);
        break;
    }
    case 0x13: ncds_core(sf, lm, 0); break; // NCDS
    case 0x14: // CDP
        mat_vec(sf, lm, 2, 3, 1, NULL);
        depth_cue_color(sf, lm);
        push_color();
        break;
    case 0x16: // NCDT
        ncds_core(sf, lm, 0); ncds_core(sf, lm, 1); ncds_core(sf, lm, 2);
        break;
    case 0x1B: nccs_core(sf, lm, 0); break; // NCCS
    case 0x1C: // CC
        mat_vec(sf, lm, 2, 3, 1, NULL);
        color_apply(sf, lm);
        push_color();
        break;
    case 0x1E: ncs_core(sf, lm, 0); break; // NCS
    case 0x20: // NCT
        ncs_core(sf, lm, 0); ncs_core(sf, lm, 1); ncs_core(sf, lm, 2);
        break;
    case 0x28: sqr(sf, lm); break;
    case 0x29: // DCPL
        depth_cue_color(sf, lm);
        push_color();
        break;
    case 0x2A: // DPCT
        for (int i = 0; i < 3; i++) {
            depth_cue(sf, lm, (int64_t)gte.rgb[0][0] << 16,
                      (int64_t)gte.rgb[0][1] << 16, (int64_t)gte.rgb[0][2] << 16);
            push_color();
        }
        break;
    case 0x2D: avsz3(); break;
    case 0x2E: avsz4(); break;
    case 0x30: // RTPT
        rtps(sf, lm, 0, 0); rtps(sf, lm, 1, 0); rtps(sf, lm, 2, 1);
        break;
    case 0x3D: gpf(sf, lm); break;
    case 0x3E: gpl(sf, lm); break;
    case 0x3F: // NCCT
        nccs_core(sf, lm, 0); nccs_core(sf, lm, 1); nccs_core(sf, lm, 2);
        break;
    default:
        break;
    }

    recalc_flag_msb();
}
