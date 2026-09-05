#ifndef VRAM_H_
#define VRAM_H_
#include <stdint.h>
#define VRAM_WIDTH  1024
#define VRAM_HEIGHT 512

typedef struct {
    // Allocation block for the 1024x512 canvas grid
    // 1024 * 512 * 2 bytes = Exactly 1 Megabyte of standard VRAM
    uint16_t canvas[VRAM_WIDTH][VRAM_HEIGHT];

    // Tracking variables for active block streams (Command 0xA0)
    uint16_t transfer_start_x;
    uint16_t transfer_start_y;
    uint16_t transfer_current_x;
    uint16_t transfer_current_y;
    uint16_t transfer_width;
    uint16_t transfer_height;

    uint8_t  is_transfer_active;
} PSX_VRAM;

PSX_VRAM vram;
#endif
