#include "script.h"
#include "pad.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

// Host unit test for script.c. It provides the pad globals/hooks and the cdrom
// entry points the script drives, so script.c links on its own.

uint16_t pad_buttons = 0xFFFF;
uint8_t pad_rstick_x = 0x80, pad_rstick_y = 0x80;
uint8_t pad_lstick_x = 0x80, pad_lstick_y = 0x80;

static int analog_flag;
void pad_toggle_analog(void) { analog_flag = !analog_flag; }
int  pad_analog_mode(void) { return analog_flag; }

static int mount_count, eject_count;
static char last_mount[256];
int cdrom_mount(const char *path)
{
    mount_count++;
    snprintf(last_mount, sizeof last_mount, "%s", path);
    return 0;
}
void cdrom_eject(void) { eject_count++; }

static int failures;
static void
check(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got 0x%X want 0x%X\n", what, got, want);
        failures++;
    }
}

int
main(void)
{
    const char *path = "_out/_test.script";

    FILE *f = fopen(path, "w");
    if (!f) { perror(path); return 1; }
    // Deliberately out of order to exercise sorting.
    fprintf(f,
            "# a mixed system-event script\n"
            "0    pad none\n"
            "10   pad right\n"
            "20   pad right cross\n"
            "30   pad lx=0x40 ly=0xC0\n"
            "5    analog on\n"
            "15   disc games/a.bin\n"
            "25   disc eject\n"
            "30   analog off\n");
    fclose(f);

    if (script_load(path) != 0) {
        fprintf(stderr, "FAIL: load\n");
        return 1;
    }
    check("active", script_active(), 1);

    // Before the first analog event: idle pad, analog off.
    script_frame(4);
    check("f4 pad idle", pad_buttons, 0xFFFF);
    check("f4 analog off", pad_analog_mode(), 0);

    // Frame 5: analog on.
    script_frame(5);
    check("f5 analog on", pad_analog_mode(), 1);

    // Frame 12: Right held, disc not inserted yet.
    script_frame(12);
    check("f12 right held", pad_buttons, (uint16_t)~PAD_BTN_RIGHT);
    check("f12 no mount yet", mount_count, 0);

    // Frame 15: disc inserted.
    script_frame(15);
    check("f15 mounted", mount_count, 1);
    check("f15 mount path", strcmp(last_mount, "games/a.bin") == 0, 1);

    // Frame 20: Right and Cross.
    script_frame(20);
    check("f20 right+cross", pad_buttons, (uint16_t)~(PAD_BTN_RIGHT | PAD_BTN_CROSS));

    // Frame 25: disc ejected.
    script_frame(25);
    check("f25 ejected", eject_count, 1);

    // Frame 30: pad stick set, analog off, buttons released.
    script_frame(30);
    check("f30 pad idle buttons", pad_buttons, 0xFFFF);
    check("f30 lx", pad_lstick_x, 0x40);
    check("f30 ly", pad_lstick_y, 0xC0);
    check("f30 analog off", pad_analog_mode(), 0);

    // A backwards frame (a reboot) replays the one-shot events.
    script_frame(0);
    check("replay: analog reset off", pad_analog_mode(), 0);
    script_frame(16);
    check("replay: mounted again", mount_count, 2);
    check("replay: analog on again", pad_analog_mode(), 1);

    remove(path);

    printf("%s (%d failure%s)\n",
           failures ? "FAILED" : "PASS: timed pad and disc events by frame",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
