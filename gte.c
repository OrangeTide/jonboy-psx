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

void
gte_execute(uint32_t instruction)
{
    // The command ops are not implemented yet. Clear FLAG so a program that
    // checks it after a command does not see stale error bits.
    (void)instruction;
    gte.flag = 0;
}
