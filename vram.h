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

    // VRAM-to-CPU read stream (Command 0xC0), drained through GPUREAD.
    uint16_t rd_start_x;
    uint16_t rd_start_y;
    uint16_t rd_current_x;
    uint16_t rd_current_y;
    uint16_t rd_width;
    uint16_t rd_height;

    uint8_t  is_read_active;
} PSX_VRAM;

extern PSX_VRAM vram;

void psx_gpu_write_pixel_payload(uint32_t two_pixels_word);
uint32_t psx_gpu_read_pixel_payload(void);
#endif
