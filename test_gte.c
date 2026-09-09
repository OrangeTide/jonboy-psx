#include "gte.h"

#include <stdio.h>
#include <stdlib.h>

// Host unit test for the GTE register file and move-op accessors (the path the
// CPU's MFC2/CFC2/MTC2/CTC2 and LWC2/SWC2 dispatch drives). It checks plain
// round-trips plus the hardware read-back quirks: sign-extension on the Z and
// IR registers, the SXY FIFO push, the IRGB/ORGB colour conversion, the
// leading-zero count, the H sign-extend quirk, and the FLAG write mask.

static int failures;

static void
check(int cond, const char *msg)
{
    if (!cond) {
        printf("  FAIL: %s\n", msg);
        failures++;
    }
}

static void
check_eq(uint32_t got, uint32_t want, const char *msg)
{
    if (got != want) {
        printf("  FAIL: %s (got 0x%08X want 0x%08X)\n", msg, got, want);
        failures++;
    }
}

// A plain data register round-trips its written value.
static void
test_data_roundtrip(void)
{
    gte_reset();
    gte_write_data(0, 0x12345678);          // VXY0
    check_eq(gte_read_data(0), 0x12345678, "VXY0 round-trip");

    gte_write_data(24, 0xDEADBEEF);         // MAC0 (full 32-bit)
    check_eq(gte_read_data(24), 0xDEADBEEF, "MAC0 round-trip");

    gte_write_data(6, 0x04030201);          // RGBC bytes
    check_eq(gte_read_data(6), 0x04030201, "RGBC round-trip");
}

// VZ and IR registers hold a signed 16-bit value and read back sign-extended.
static void
test_sign_extend(void)
{
    gte_reset();
    gte_write_data(1, 0x0000FFFF);          // VZ0 = -1
    check_eq(gte_read_data(1), 0xFFFFFFFF, "VZ0 sign-extends");

    gte_write_data(9, 0x00008000);          // IR1 = -32768
    check_eq(gte_read_data(9), 0xFFFF8000, "IR1 sign-extends");

    // The high half of a VZ write is ignored (only the low 16 bits are stored).
    gte_write_data(3, 0xABCD0001);          // VZ1 = 1
    check_eq(gte_read_data(3), 0x00000001, "VZ1 keeps only low 16 bits");
}

// Writing SXYP (reg 15) pushes the screen-coordinate FIFO; reg 14 and reg 15
// both read the top entry.
static void
test_sxy_fifo(void)
{
    gte_reset();
    gte_write_data(12, 0x00010002);         // SXY0
    gte_write_data(13, 0x00030004);         // SXY1
    gte_write_data(14, 0x00050006);         // SXY2

    gte_write_data(15, 0x00070008);         // SXYP push
    check_eq(gte_read_data(12), 0x00030004, "SXYP push: SXY1 -> SXY0");
    check_eq(gte_read_data(13), 0x00050006, "SXYP push: SXY2 -> SXY1");
    check_eq(gte_read_data(14), 0x00070008, "SXYP push: new -> SXY2");
    check_eq(gte_read_data(15), 0x00070008, "SXYP reads top of FIFO");
}

// IRGB (28) write splits a 15-bit colour into IR1..IR3; IRGB/ORGB read back the
// saturated 5-bit-per-channel conversion of IR1..IR3.
static void
test_irgb(void)
{
    gte_reset();
    gte_write_data(28, (10) | (20 << 5) | (30 << 10));
    check_eq(gte_read_data(9),  (uint32_t)(10 << 7), "IRGB write sets IR1");
    check_eq(gte_read_data(10), (uint32_t)(20 << 7), "IRGB write sets IR2");
    check_eq(gte_read_data(11), (uint32_t)(30 << 7), "IRGB write sets IR3");
    check_eq(gte_read_data(28), (10) | (20 << 5) | (30 << 10), "IRGB read converts back");
    check_eq(gte_read_data(29), (10) | (20 << 5) | (30 << 10), "ORGB matches IRGB read");

    // ORGB saturates each channel to 0..0x1F.
    gte_reset();
    gte_write_data(9, 0x7F00);              // IR1 >> 7 = 254 -> clamp 0x1F
    gte_write_data(10, 0xFF80);             // IR2 negative -> clamp 0
    check_eq(gte_read_data(29) & 0x1F, 0x1F, "ORGB clamps high to 0x1F");
    check_eq((gte_read_data(29) >> 5) & 0x1F, 0, "ORGB clamps negative to 0");
}

// LZCS write latches the value and LZCR reports its leading zero/one count.
static void
test_lzc(void)
{
    gte_reset();
    gte_write_data(30, 0x00000001);
    check_eq(gte_read_data(30), 0x00000001, "LZCS reads back as written");
    check_eq(gte_read_data(31), 31, "LZCR counts 31 leading zeros of 0x1");

    gte_write_data(30, 0xFFFFFFFF);
    check_eq(gte_read_data(31), 32, "LZCR counts 32 leading ones");

    gte_write_data(30, 0x00000000);
    check_eq(gte_read_data(31), 32, "LZCR counts 32 leading zeros of 0");

    gte_write_data(30, 0x80000000);
    check_eq(gte_read_data(31), 1, "LZCR counts 1 leading one");

    // LZCR is read-only; a write does not change it.
    gte_write_data(30, 0x0000000F);         // LZCR now 28
    gte_write_data(31, 0x12345678);
    check_eq(gte_read_data(31), 28, "LZCR ignores writes");
}

// Control-register matrix packing: two 16-bit halves per word, RT33 alone.
static void
test_control_matrix(void)
{
    gte_reset();
    gte_write_control(0, 0x00020001);       // RT11=1, RT12=2
    gte_write_control(1, 0x00040003);       // RT13=3, RT21=4
    check_eq(gte_read_control(0), 0x00020001, "RT11/RT12 pack");
    check_eq(gte_read_control(1), 0x00040003, "RT13/RT21 pack");

    gte_write_control(4, 0x0000FFFF);       // RT33 = -1
    check_eq(gte_read_control(4), 0xFFFFFFFF, "RT33 sign-extends");

    gte_write_control(5, 0x7FFFFFFF);       // TRX full 32-bit
    check_eq(gte_read_control(5), 0x7FFFFFFF, "TRX round-trip");
}

// H (reg 26) reads back sign-extended from 16 bits, a documented quirk.
static void
test_h_quirk(void)
{
    gte_reset();
    gte_write_control(26, 0x0000F000);      // H = 0xF000
    check_eq(gte_read_control(26), 0xFFFFF000, "H reads back sign-extended");
}

// FLAG write keeps only bits 12..30 and recomputes the bit-31 error summary.
static void
test_flag(void)
{
    gte_reset();
    gte_write_control(31, 0xFFFFFFFF);
    check((gte_read_control(31) & 0x00000FFF) == 0, "FLAG masks off bits 0..11");
    check((gte_read_control(31) & 0x80000000u) != 0, "FLAG bit 31 set from error bits");

    gte_reset();
    gte_write_control(31, 0x00001000);      // a low defined bit, not an error bit
    check((gte_read_control(31) & 0x80000000u) == 0, "FLAG bit 31 clear without error bits");
}

int
main(void)
{
    test_data_roundtrip();
    test_sign_extend();
    test_sxy_fifo();
    test_irgb();
    test_lzc();
    test_control_matrix();
    test_h_quirk();
    test_flag();

    if (failures) {
        printf("GTE: %d test(s) failed\n", failures);
        return EXIT_FAILURE;
    }

    printf("GTE: all tests passed\n");
    return EXIT_SUCCESS;
}
