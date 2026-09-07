// In-emulation game menu. Runs as a PS-EXE sideloaded once the kernel is up.
// It enumerates host images over the loader backdoor, draws the list with an
// embedded font, and boots the highlighted entry when Cross is pressed.

#include "font8x8.h"

#define GP0 (*(volatile unsigned int *)0xBF801810)
#define GP1 (*(volatile unsigned int *)0xBF801814)

// SIO0 (controller port) registers.
#define JOY_DATA (*(volatile unsigned char *)0xBF801040)
#define JOY_STAT (*(volatile unsigned short *)0xBF801044)
#define JOY_CTRL (*(volatile unsigned short *)0xBF80104A)

// Loader backdoor registers.
#define LDR_CMD   (*(volatile unsigned int *)0xBF802000)
#define LDR_INDEX (*(volatile unsigned int *)0xBF802004)
#define LDR_DATA  (*(volatile unsigned int *)0xBF802008)
#define LDR_CMD_REFRESH 1u
#define LDR_CMD_NAME    2u
#define LDR_CMD_BOOT    3u
#define LDR_STAT_DATA   2u

// Digital pad buttons, active low.
#define BTN_UP    (1u << 4)
#define BTN_RIGHT (1u << 5)
#define BTN_DOWN  (1u << 6)
#define BTN_LEFT  (1u << 7)
#define BTN_CROSS (1u << 14)

#define MAX_ENTRIES 24
#define MAX_NAME    40

static char names[MAX_ENTRIES][MAX_NAME + 1];
static int entry_count;

// ---- drawing -------------------------------------------------------------

static void
fill_rect(unsigned rgb, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    GP0 = 0x02000000u | (rgb & 0xFFFFFFu);
    GP0 = ((unsigned)y << 16) | (unsigned)(x & 0xFFFF);
    GP0 = ((unsigned)h << 16) | (unsigned)w;
}

// Draw one glyph as horizontal pixel runs, so each row is a few fills.
static void
draw_char(int px, int py, unsigned char c, unsigned rgb)
{
    const char *g = font8x8_basic[c & 0x7F];

    for (int row = 0; row < 8; row++) {
        unsigned bits = (unsigned char)g[row];
        int col = 0;

        while (col < 8) {
            if (bits & (1u << col)) {
                int run = 1;
                while (col + run < 8 && (bits & (1u << (col + run)))) {
                    run++;
                }
                fill_rect(rgb, px + col, py + row, run, 1);
                col += run;
            } else {
                col++;
            }
        }
    }
}

static void
draw_text(int px, int py, const char *s, unsigned rgb)
{
    for (int i = 0; s[i]; i++) {
        draw_char(px + i * 8, py, (unsigned char)s[i], rgb);
    }
}

// ---- backdoor ------------------------------------------------------------

static void
enumerate(void)
{
    LDR_CMD = LDR_CMD_REFRESH;
    entry_count = (int)LDR_INDEX;

    if (entry_count > MAX_ENTRIES) {
        entry_count = MAX_ENTRIES;
    }

    for (int i = 0; i < entry_count; i++) {
        LDR_INDEX = (unsigned)i;
        LDR_CMD = LDR_CMD_NAME;

        int n = 0;
        while ((LDR_CMD & LDR_STAT_DATA) && n < MAX_NAME) {
            names[i][n++] = (char)LDR_DATA;
        }
        names[i][n] = '\0';
    }
}

// ---- input ---------------------------------------------------------------

// Run one controller read and return the 16-bit button word (active low).
static unsigned
read_pad(void)
{
    unsigned char resp[5];
    const unsigned char tx[5] = { 0x01, 0x42, 0x00, 0x00, 0x00 };

    JOY_CTRL = 0x0003; // TXEN + select slot 1

    for (int i = 0; i < 5; i++) {
        JOY_DATA = tx[i];
        while (!(JOY_STAT & 0x0002)) {
        }
        resp[i] = JOY_DATA;
    }

    JOY_CTRL = 0x0000; // deselect
    return (unsigned)resp[3] | ((unsigned)resp[4] << 8);
}

// ---- gpu setup + menu ----------------------------------------------------

static void
gpu_init(void)
{
    GP1 = 0x00000000;                 // reset
    GP1 = 0x03000000;                 // display on
    GP1 = 0x08000001;                 // 320x240, 15bpp, NTSC
    GP1 = 0x05000000;                 // display start (0,0)

    GP0 = 0xE1000000;                 // draw mode
    GP0 = 0xE3000000;                 // draw area top-left (0,0)
    GP0 = 0xE4000000u | 319u | (239u << 10); // bottom-right
    GP0 = 0xE5000000;                 // draw offset (0,0)
}

static void
draw_menu(int sel)
{
    fill_rect(0x201000, 0, 0, 320, 240);        // background
    draw_text(16, 8, "JONBOY LOADER", 0x30C0FF);
    fill_rect(0x30C0FF, 16, 18, 288, 1);

    if (entry_count == 0) {
        draw_text(16, 40, "NO IMAGES IN GAMES DIR", 0x8080FF);
        return;
    }

    for (int i = 0; i < entry_count; i++) {
        int y = 28 + i * 10;

        if (i == sel) {
            fill_rect(0x603000, 12, y - 1, 296, 9); // highlight bar
            draw_text(16, y, names[i], 0xFFFFFF);
        } else {
            draw_text(16, y, names[i], 0xB0B0B0);
        }
    }
}

int
main(void)
{
    gpu_init();
    enumerate();

    int sel = 0;
    unsigned prev = 0xFFFF;
    int need_draw = 1;

    for (;;) {
        if (need_draw) {
            draw_menu(sel);
            need_draw = 0;
        }

        unsigned cur = read_pad();
        unsigned pressed = prev & ~cur; // bits that went 1 -> 0 (newly held)
        prev = cur;

        if (entry_count > 0) {
            if (pressed & BTN_DOWN) {
                sel = (sel + 1) % entry_count;
                need_draw = 1;
            }
            if (pressed & BTN_UP) {
                sel = (sel - 1 + entry_count) % entry_count;
                need_draw = 1;
            }
            if (pressed & BTN_CROSS) {
                LDR_INDEX = (unsigned)sel;
                LDR_CMD = LDR_CMD_BOOT; // emulator hijacks PC to the game
                for (;;) {
                }
            }
        }

        // Crude debounce so one press moves one step.
        for (volatile int d = 0; d < 30000; d++) {
        }
    }

    return 0;
}
