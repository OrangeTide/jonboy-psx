#ifndef GPU_H_
#define GPU_H_
#include <stdint.h>

#define GPU_FIFO_MAX 64

typedef struct {
    // Structural Hardware Buffers
    uint32_t command_fifo[GPU_FIFO_MAX];
    uint32_t fifo_words_count;
    uint32_t current_packet_length;

    // Core Engine Registers
    uint32_t gpu_status; // Exposed via reading 0x1F801814

    // Rendering state set by the GP0 environment commands.
    int32_t draw_offset_x; // added to every primitive vertex (GP0 0xE5)
    int32_t draw_offset_y;
    int32_t draw_x0;       // drawing area clip, inclusive (GP0 0xE3/0xE4)
    int32_t draw_y0;
    int32_t draw_x1;
    int32_t draw_y1;

    // Current texture page, from GP0 0xE1 or a textured primitive's own
    // texpage word. Base is in VRAM pixels; depth selects the texel format.
    int32_t tex_page_x;
    int32_t tex_page_y;
    int32_t tex_depth;     // 0 = 4-bit CLUT, 1 = 8-bit CLUT, 2/3 = 15-bit
    int32_t tex_semi_mode; // semi-transparency blend function (0-3)

    // Texture window (GP0 0xE2), in 8-texel units. Repeats a sub-region of
    // the texture page across texture coordinates.
    int32_t tw_mask_x;
    int32_t tw_mask_y;
    int32_t tw_off_x;
    int32_t tw_off_y;
} PSX_GPU;

extern PSX_GPU gpu;

void psx_gpu_reset(void);
void psx_gpu_send_command_gp0(uint32_t value);
void psx_gpu_send_command_gp1(uint32_t value);
uint32_t psx_gpu_read_vram_or_gp0();
uint32_t psx_gpu_get_status();
#endif
