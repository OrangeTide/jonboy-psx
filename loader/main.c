// Minimal render test: bring up the GPU and draw into VRAM(0,0) so the emulator
// framebuffer dump shows it. Proves the crt0/linker/PS-EXE build and the GPU
// command path before adding the font and menu.

#define GP0 (*(volatile unsigned int *)0xBF801810)
#define GP1 (*(volatile unsigned int *)0xBF801814)

static void
fill_rect(unsigned rgb, unsigned x, unsigned y, unsigned w, unsigned h)
{
    GP0 = 0x02000000u | (rgb & 0xFFFFFFu);
    GP0 = (y << 16) | x;
    GP0 = (h << 16) | w;
}

int
main(void)
{
    GP1 = 0x00000000;                 // reset GPU
    GP1 = 0x03000000;                 // display enable
    GP1 = 0x08000001;                 // 320x240, 15bpp, NTSC
    GP1 = 0x05000000;                 // display area starts at VRAM (0,0)

    GP0 = 0xE1000000;                 // draw mode
    GP0 = 0xE3000000;                 // draw area top-left (0,0)
    GP0 = 0xE4000000u | 319u | (239u << 10); // draw area bottom-right
    GP0 = 0xE5000000;                 // draw offset (0,0)

    fill_rect(0x401000, 0, 0, 320, 240);   // dark blue background
    fill_rect(0x0000FF, 32, 24, 96, 32);   // red bar
    fill_rect(0x00FF00, 32, 72, 96, 32);   // green bar
    fill_rect(0xFFFFFF, 32, 120, 96, 32);  // white bar

    for (;;) {
    }

    return 0;
}
