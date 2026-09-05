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
} PSX_GPU;

extern PSX_GPU gpu;

#endif
