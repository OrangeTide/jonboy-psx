#include "pad.h"
#include "cpu.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Host unit test for the controller port (pad.c): the digital pad poll, and the
// DualShock analog mode reached both through the config command sequence and the
// ANALOG toggle. It drives the SIO0 registers the way the BIOS/game does.
//
// pad.c raises the acknowledge interrupt through the CPU interrupt-status word,
// so the cpu global is provided here. It is also linked with mc.c (memory card),
// which reports no card, so the 0x81 address path stays inert.

R3051_CPU cpu;

#define REG_DATA (PAD_BASE + 0x0)
#define REG_CTRL (PAD_BASE + 0xA)

static int failures;

static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got 0x%02X want 0x%02X\n", what, got, want);
        failures++;
    }
}

// Run one transaction: deassert then assert select (a rising edge resets the
// transaction), then clock each byte of send[] and record the response.
static void
run(const uint8_t *send, uint8_t *recv, int n)
{
    psx_pad_write(REG_CTRL, 0x0000, 2);
    psx_pad_write(REG_CTRL, 0x0002, 2); // CTRL_SELECT, slot 1
    for (int i = 0; i < n; i++) {
        psx_pad_write(REG_DATA, send[i], 1);
        recv[i] = (uint8_t)psx_pad_read(REG_DATA, 1);
    }
}

int
main(void)
{
    uint8_t recv[16];

    psx_pad_reset();
    pad_buttons = 0xFFFF;

    // ---- Digital pad poll (ID 0x41) -------------------------------------------
    {
        uint8_t send[5] = { 0x01, 0x42, 0, 0, 0 };
        run(send, recv, 5);
        check("digital addr", recv[0], 0xFF);
        check("digital ID", recv[1], 0x41);
        check("digital 5A", recv[2], 0x5A);
        check("digital btn lo", recv[3], 0xFF);
        check("digital btn hi", recv[4], 0xFF);
    }

    // Pressing a button clears its active-low bit in the report.
    pad_buttons = (uint16_t)~PAD_BTN_CROSS; // Cross held
    {
        uint8_t send[5] = { 0x01, 0x42, 0, 0, 0 };
        run(send, recv, 5);
        check("btn hi has Cross low", recv[4], (uint8_t)((~PAD_BTN_CROSS) >> 8));
    }
    pad_buttons = 0xFFFF;

    // ---- Enter analog mode via the config command sequence --------------------
    {
        uint8_t enter[9]  = { 0x01, 0x43, 0x00, 0x01, 0, 0, 0, 0, 0 };
        uint8_t setan[9]  = { 0x01, 0x44, 0x00, 0x01, 0x03, 0, 0, 0, 0 };
        uint8_t status[9] = { 0x01, 0x45, 0x00, 0, 0, 0, 0, 0, 0 };
        uint8_t exit9[9]  = { 0x01, 0x43, 0x00, 0x00, 0, 0, 0, 0, 0 };

        run(enter, recv, 9);
        // In config mode the status query reports the model and analog flag:
        // F3, 5A, 01, 02, <analog>, 02, 01, 00.
        run(status, recv, 9);
        check("config ID (0xF3)", recv[1], 0xF3);
        check("config 5A", recv[2], 0x5A);
        check("status byte", recv[3], 0x01);
        check("status byte 2", recv[4], 0x02);
        check("status analog flag (off)", recv[5], 0x00);

        run(setan, recv, 9);
        run(status, recv, 9);
        check("status analog flag (on)", recv[5], 0x01);

        run(exit9, recv, 9);
    }

    // ---- Analog poll (ID 0x73) with stick axes --------------------------------
    pad_rstick_x = 0x11;
    pad_rstick_y = 0x22;
    pad_lstick_x = 0x33;
    pad_lstick_y = 0x44;
    {
        uint8_t send[9] = { 0x01, 0x42, 0, 0, 0, 0, 0, 0, 0 };
        run(send, recv, 9);
        check("analog ID", recv[1], 0x73);
        check("analog 5A", recv[2], 0x5A);
        check("analog btn lo", recv[3], 0xFF);
        check("analog btn hi", recv[4], 0xFF);
        check("analog rstick X", recv[5], 0x11);
        check("analog rstick Y", recv[6], 0x22);
        check("analog lstick X", recv[7], 0x33);
        check("analog lstick Y", recv[8], 0x44);
    }

    // ---- The ANALOG button toggles back to digital ----------------------------
    check("analog mode set", pad_analog_mode(), 1);
    pad_toggle_analog();
    check("analog mode toggled off", pad_analog_mode(), 0);
    {
        uint8_t send[5] = { 0x01, 0x42, 0, 0, 0 };
        run(send, recv, 5);
        check("digital ID after toggle", recv[1], 0x41);
    }
    pad_toggle_analog();
    {
        uint8_t send[9] = { 0x01, 0x42, 0, 0, 0, 0, 0, 0, 0 };
        run(send, recv, 9);
        check("analog ID after toggle", recv[1], 0x73);
    }

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "PASS: digital pad and DualShock analog mode",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
