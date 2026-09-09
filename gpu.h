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

    // Poly-line parsing state (GP0 0x48-0x4F, 0x58-0x5F). Poly-lines have no
    // fixed length, so their vertices are consumed one at a time and each new
    // segment is drawn immediately, until a termination code arrives.
    int32_t pl_active;
    int32_t pl_shaded;
    int32_t pl_semi;
    int32_t pl_semi_mode;
    int32_t pl_count;        // vertices received so far
    int32_t pl_expect_color; // shaded: next word is a color, not a vertex
    int32_t pl_prev_x, pl_prev_y;
    int32_t pl_prev_r, pl_prev_g, pl_prev_b;
    int32_t pl_cur_r, pl_cur_g, pl_cur_b;

    // Display output (GP1 03/05/08): the VRAM region shown on screen and how.
    int32_t disp_x;      // VRAM column of the top-left displayed pixel
    int32_t disp_y;      // VRAM row of the top-left displayed pixel
    int32_t disp_w;      // displayed width in pixels (from horizontal res)
    int32_t disp_h;      // displayed height in pixels (from vertical res)
    int32_t disp_blank;  // 1 = display disabled (blank screen), 0 = enabled
    int32_t disp_24bpp;  // 1 = 24-bit color mode, 0 = 15-bit
    int32_t disp_range_y1; // GP1(07) first displayed scanline
    int32_t disp_range_y2; // GP1(07) scanline just past the last displayed one
} PSX_GPU;

extern PSX_GPU gpu;

// Visible height in VRAM rows implied by the GP1(07) vertical display range,
// clamped to the mode height (disp_h). A game that programs a shorter range than
// the mode allows (for example 224 lines in a 240-line mode) leaves stale VRAM
// below the picture; presenting only the active rows keeps that garbage off
// screen the way a TV's overscan does.
int psx_gpu_active_height(void);

void psx_gpu_reset(void);
void psx_gpu_send_command_gp0(uint32_t value);
void psx_gpu_send_command_gp1(uint32_t value);
uint32_t psx_gpu_read_vram_or_gp0();
uint32_t psx_gpu_get_status();
#endif
